using System;
using System.Collections.ObjectModel;
using System.ComponentModel;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Reflection;
using System.Threading.Tasks;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Interop;
using System.Windows.Media.Imaging;
using SunshineWindowController.Models;
using SunshineWindowController.Services;
using Forms = System.Windows.Forms;

namespace SunshineWindowController
{
    /// <summary>
    /// 主窗口：列出桌面和活跃窗口，点击切换 Sunshine 录制目标。
    /// </summary>
    public partial class MainWindow : Window
    {
        /// <summary>Sunshine Web UI 的 PIN 页路径（服务端路由 ^/pin/?$）。</summary>
        private const string SunshinePinPath = "/pin";

        private readonly AppSettings _settings;
        private bool _isRefreshing;
        private bool _suppressSelection;
        private bool _isLoaded;
        private int _refreshGeneration;

        /// <summary>定时刷新窗口列表的计时器（每 3 秒一次）。</summary>
        private readonly System.Windows.Threading.DispatcherTimer _refreshTimer;

        /// <summary>焦点伪造注入/开关操作进行中标记，期间暂停定时刷新。</summary>
        private bool _spoofBusy;

        /// <summary>每个进程当前的 hook 状态（pid -> hooked）。用于在窗口列表刷新后恢复按钮文本。</summary>
        private readonly System.Collections.Generic.Dictionary<uint, bool> _hookStateByPid =
            new System.Collections.Generic.Dictionary<uint, bool>();

        /// <summary>系统托盘图标，常驻提供打开主页 / PIN 页 / 显示主窗口 / 退出。</summary>
        private Forms.NotifyIcon _trayIcon;

        /// <summary>托盘图标右键菜单。</summary>
        private Forms.ContextMenuStrip _trayMenu;

        /// <summary>正在真正退出（用于区分"关窗口=隐藏到托盘"与"退出程序"）。</summary>
        private bool _isExiting;

        /// <summary>是否已经提示过"窗口已隐藏到托盘"。</summary>
        private bool _trayHintShown;

        /// <summary>窗口消息钩子，用于接收其它实例发来的"显示主窗口"请求。</summary>
        private HwndSource _hwndSource;

        /// <summary>"显示主窗口"自定义窗口消息号。</summary>
        private uint _showMainWindowMessage;

        public ObservableCollection<CaptureTarget> Targets { get; } = new ObservableCollection<CaptureTarget>();

        public MainWindow()
        {
            InitializeComponent();
            DataContext = this;

            _settings = AppSettings.Load();

            // 填充连接设置
            BaseUrlBox.Text = _settings.BaseUrl;
            UsernameBox.Text = _settings.Username;
            PasswordBox.Password = _settings.Password;

            PopulateSunshinePaths();

            // 初始化配置文件
            InitializeConfigFile();

            // 焦点伪造选项
            LoadFocusOptions();

            // 托盘图标与右键菜单
            InitializeTrayIcon();

            // 此后下拉框的选中变化即视为用户操作并立即应用
            _isLoaded = true;

            // 初始刷新窗口列表
            _ = RefreshTargetsAsync();

            // 定时刷新窗口列表
            _refreshTimer = new System.Windows.Threading.DispatcherTimer
            {
                Interval = TimeSpan.FromSeconds(3)
            };
            _refreshTimer.Tick += RefreshTimer_Tick;
            _refreshTimer.Start();
        }

        /// <summary>
        /// 建立托盘图标与右键菜单：打开 YuanSunshine（Web UI 主页）、PIN（Web UI 的 PIN 页）、
        /// 显示主窗口、退出。
        /// </summary>
        private void InitializeTrayIcon()
        {
            _trayMenu = new Forms.ContextMenuStrip();
            _trayMenu.Items.Add("打开 YuanSunshine", null, (s, e) => OpenSunshineWeb());
            _trayMenu.Items.Add("PIN", null, (s, e) => OpenSunshineWeb(SunshinePinPath));

            _trayMenu.Items.Add(new Forms.ToolStripSeparator());

            _trayMenu.Items.Add("显示主窗口", null, (s, e) => ShowMainWindow());
            _trayMenu.Items.Add("退出", null, (s, e) => ExitApplication());

            _trayIcon = new Forms.NotifyIcon
            {
                Icon = LoadTrayIcon(),
                Text = "Sunshine 窗口控制器",
                ContextMenuStrip = _trayMenu,
                Visible = true,
            };
            _trayIcon.DoubleClick += (s, e) => ShowMainWindow();
        }

        /// <summary>
        /// 取托盘图标：优先使用本程序自身嵌入的图标，其次 1up.ico，最后用系统默认图标。
        /// </summary>
        /// <returns>可用的托盘图标。</returns>
        private static System.Drawing.Icon LoadTrayIcon()
        {
            try
            {
                var exe = Assembly.GetExecutingAssembly().Location;
                var icon = System.Drawing.Icon.ExtractAssociatedIcon(exe);
                if (icon != null)
                    return icon;
            }
            catch (Exception)
            {
                // 继续尝试 1up.ico
            }

            try
            {
                var icoPath = Path.Combine(AppDomain.CurrentDomain.BaseDirectory, "1up.ico");
                if (File.Exists(icoPath))
                    return new System.Drawing.Icon(icoPath);
            }
            catch (Exception)
            {
                // 继续使用系统默认图标
            }

            return System.Drawing.SystemIcons.Application;
        }

        /// <summary>
        /// 从托盘或其它实例请求显示主窗口：恢复显示并置前。
        /// </summary>
        private void ShowMainWindow()
        {
            Dispatcher.Invoke(() =>
            {
                if (!IsVisible)
                    Show();

                if (WindowState == WindowState.Minimized)
                    WindowState = WindowState.Normal;

                Activate();
            });
        }

        /// <summary>
        /// 真正退出程序：隐藏并释放托盘图标后关闭应用程序。
        /// </summary>
        private void ExitApplication()
        {
            _isExiting = true;

            try
            {
                _trayIcon.Visible = false;
                _trayIcon.Dispose();
                _trayMenu.Dispose();
            }
            catch (Exception)
            {
                // 图标已释放等情况忽略
            }

            Application.Current.Shutdown();
        }

        /// <summary>
        /// 关闭窗口时隐藏到托盘而不是退出程序；真正退出走托盘菜单的"退出"。
        /// </summary>
        /// <param name="e">关闭事件参数。</param>
        protected override void OnClosing(CancelEventArgs e)
        {
            if (!_isExiting)
            {
                e.Cancel = true;
                Hide();

                if (!_trayHintShown)
                {
                    _trayHintShown = true;
                    try
                    {
                        _trayIcon.ShowBalloonTip(3000, "Sunshine 窗口控制器",
                            "已隐藏到托盘：右键托盘图标可打开 sunshine 主页、PIN 页、显示主窗口或退出。",
                            Forms.ToolTipIcon.Info);
                    }
                    catch (Exception)
                    {
                        // 气泡提示失败不影响隐藏
                    }
                }

                return;
            }

            base.OnClosing(e);
        }

        /// <summary>
        /// 窗口句柄创建后挂上消息钩子，接收其它实例的"显示主窗口"请求。
        /// </summary>
        /// <param name="e">事件参数。</param>
        protected override void OnSourceInitialized(EventArgs e)
        {
            base.OnSourceInitialized(e);

            _showMainWindowMessage = RegisterWindowMessage(App.ShowMainWindowMessageName);
            _hwndSource = HwndSource.FromHwnd(new WindowInteropHelper(this).Handle);
            _hwndSource?.AddHook(WindowMessageHook);
        }

        /// <summary>
        /// 窗口消息钩子：处理"显示主窗口"自定义消息。
        /// </summary>
        /// <param name="hwnd">窗口句柄。</param>
        /// <param name="msg">消息号。</param>
        /// <param name="wParam">消息参数。</param>
        /// <param name="lParam">消息参数。</param>
        /// <param name="handled">是否已处理。</param>
        /// <returns>处理结果。</returns>
        private IntPtr WindowMessageHook(IntPtr hwnd, int msg, IntPtr wParam, IntPtr lParam, ref bool handled)
        {
            if (_showMainWindowMessage != 0 && (uint)msg == _showMainWindowMessage)
            {
                ShowMainWindow();
                handled = true;
            }

            return IntPtr.Zero;
        }

        [System.Runtime.InteropServices.DllImport("user32.dll", CharSet = System.Runtime.InteropServices.CharSet.Unicode)]
        private static extern uint RegisterWindowMessage(string message);

        /// <summary>
        /// 定时刷新窗口列表：窗口不活跃（失去焦点或最小化）、用户正按住鼠标交互、
        /// 或焦点伪造操作进行中时跳过本轮。
        /// </summary>
        private void RefreshTimer_Tick(object sender, EventArgs e)
        {
            if (!IsActive)
                return;
            if (System.Windows.Input.Mouse.LeftButton == System.Windows.Input.MouseButtonState.Pressed)
                return;
            if (_spoofBusy)
                return;
            _ = RefreshTargetsAsync();
        }

        /// <summary>
        /// 窗口关闭时停止定时刷新计时器。
        /// </summary>
        protected override void OnClosed(EventArgs e)
        {
            _refreshTimer?.Stop();
            base.OnClosed(e);
        }

        /// <summary>
        /// 从配置文件加载焦点伪造选项到复选框。
        /// </summary>
        private void LoadFocusOptions()
        {
            SuspendOnPatchCheck.IsChecked = FocusSpoof.GetOption("SuspendThreadsOnPatch", _settings.SuspendThreadsOnPatch ? 1 : 0) != 0;
            EnableSubclassCheck.IsChecked = FocusSpoof.GetOption("EnableSubclass", _settings.EnableSubclass ? 1 : 0) != 0;
            BlockPollApisCheck.IsChecked = FocusSpoof.GetOption("BlockPollingApis", _settings.BlockPollingApis ? 1 : 0) != 0;
            BlockWmInputCheck.IsChecked = FocusSpoof.GetOption("BlockWmInput", _settings.BlockWmInput ? 1 : 0) != 0;
            BlockRawApisCheck.IsChecked = FocusSpoof.GetOption("BlockRawInputApis", _settings.BlockRawInputApis ? 1 : 0) != 0;
            BlockCursorHideCheck.IsChecked = FocusSpoof.GetOption("BlockCursorHide", _settings.BlockCursorHide ? 1 : 0) != 0;
            BlockCursorLockCheck.IsChecked = FocusSpoof.GetOption("BlockCursorLock", _settings.BlockCursorLock ? 1 : 0) != 0;
            DisableAllCheck.IsChecked = FocusSpoof.GetOption("DisableFocusSpoof", _settings.DisableAll ? 1 : 0) != 0;

            // XInput 手柄改写选项（与焦点伪造选项同一配置文件）
            XInputEnabledCheck.IsChecked = FocusSpoof.GetOption("XInputRewrite", _settings.XInputEnabled ? 1 : 0) != 0;
            XInputPortCombo.ItemsSource = AppSettings.XInputPortPresets;
            var savedPort = FocusSpoof.GetOption("XInputUdpPort", _settings.XInputPort);
            XInputPortCombo.SelectedItem = AppSettings.XInputPortPresets.Contains(savedPort) ? savedPort : _settings.XInputPort;
        }

        /// <summary>
        /// 初始化配置文件（如果不存在）。
        /// </summary>
        private void InitializeConfigFile()
        {
            try
            {
                var dir = Path.GetDirectoryName(FocusSpoof.ConfigPath);
                if (!Directory.Exists(dir))
                    Directory.CreateDirectory(dir);

                if (!File.Exists(FocusSpoof.ConfigPath))
                {
                    var lines = new System.Collections.Generic.List<string>
                    {
                        "SuspendThreadsOnPatch=" + (_settings.SuspendThreadsOnPatch ? 1 : 0),
                        "EnableSubclass=" + (_settings.EnableSubclass ? 1 : 0),
                        "BlockPollingApis=" + (_settings.BlockPollingApis ? 1 : 0),
                        "BlockWmInput=" + (_settings.BlockWmInput ? 1 : 0),
                        "BlockRawInputApis=" + (_settings.BlockRawInputApis ? 1 : 0),
                        "BlockCursorHide=" + (_settings.BlockCursorHide ? 1 : 0),
                        "BlockCursorLock=" + (_settings.BlockCursorLock ? 1 : 0),
                        "DisableFocusSpoof=" + (_settings.DisableAll ? 1 : 0),
                        "XInputRewrite=" + (_settings.XInputEnabled ? 1 : 0),
                        "XInputUdpPort=" + _settings.XInputPort,
                    };
                    File.WriteAllLines(FocusSpoof.ConfigPath, lines);
                }
            }
            catch (Exception)
            {
                // 静默失败
            }
        }

        private void FocusOption_Changed(object sender, RoutedEventArgs e)
        {
            if (sender is CheckBox box && box.Tag is string name && !string.IsNullOrEmpty(name))
            {
                // 保存到配置文件（DLL 从配置文件读取）
                FocusSpoof.SetOption(name, box.IsChecked == true ? 1 : 0);

                // 同时更新 AppSettings（用于持久化）
                if (_settings != null)
                {
                    UpdateSettingsFromCheckbox(box, name);
                    _settings.Save();
                }
            }
        }

        /// <summary>
        /// 根据复选框状态更新 AppSettings 对象。
        /// </summary>
        private void UpdateSettingsFromCheckbox(CheckBox box, string optionName)
        {
            var value = box.IsChecked == true;
            switch (optionName)
            {
                case "SuspendThreadsOnPatch":
                    _settings.SuspendThreadsOnPatch = value;
                    break;
                case "EnableSubclass":
                    _settings.EnableSubclass = value;
                    break;
                case "BlockPollingApis":
                    _settings.BlockPollingApis = value;
                    break;
                case "BlockWmInput":
                    _settings.BlockWmInput = value;
                    break;
                case "BlockRawInputApis":
                    _settings.BlockRawInputApis = value;
                    break;
                case "BlockCursorHide":
                    _settings.BlockCursorHide = value;
                    break;
                case "BlockCursorLock":
                    _settings.BlockCursorLock = value;
                    break;
                case "DisableFocusSpoof":
                    _settings.DisableAll = value;
                    break;
                case "XInputRewrite":
                    _settings.XInputEnabled = value;
                    break;
            }
        }

        /// <summary>
        /// 从当前设置生成发送给 Sunshine /api/capture-window 的 xinput_delivery 值。
        /// 策略固定为进程模式（仅针对被注入的目标进程），端口固定为
        /// AppSettings.XInputPort（下拉框预设之一）。
        /// </summary>
        private string BuildXinputDeliveryValue()
        {
            return _settings.XInputEnabled ? "process" : "off";
        }

        private SunshineService CreateService()
        {
            return new SunshineService
            {
                BaseUrl = BaseUrlBox.Text.Trim(),
                Username = UsernameBox.Text.Trim(),
                Password = PasswordBox.Password,
            };
        }

        /// <summary>
        /// 填充 sunshine.exe 路径下拉框：持久化路径优先，默认相对路径 ./sunshine.exe。
        /// </summary>
        private void PopulateSunshinePaths()
        {
            var choices = new System.Collections.Generic.List<string>();

            // 已运行的 Sunshine 实例
            var running = SunshineService.RunningPath();
            if (!string.IsNullOrWhiteSpace(running))
                choices.Add(running);

            // 标准安装路径
            foreach (var path in new[]
            {
                @"C:\Program Files\Sunshine\sunshine.exe",
                @"C:\Program Files\Sunshine_Next\sunshine.exe",
            })
            {
                if (System.IO.File.Exists(path))
                    choices.Add(path);
            }

            // 持久化的路径优先
            if (!string.IsNullOrWhiteSpace(_settings.SunshineExePath))
                choices.Insert(0, _settings.SunshineExePath);

            // 默认相对路径兜底
            if (!choices.Contains(SunshineService.DefaultExePath))
                choices.Add(SunshineService.DefaultExePath);

            foreach (var path in choices)
            {
                if (!SunshinePathBox.Items.Contains(path))
                    SunshinePathBox.Items.Add(path);
            }

            SunshinePathBox.Text = string.IsNullOrWhiteSpace(_settings.SunshineExePath)
                ? SunshineService.DefaultExePath
                : _settings.SunshineExePath;
        }

        private async void RefreshButton_Click(object sender, RoutedEventArgs e)
        {
            _suppressSelection = true;
            RefreshButton.IsEnabled = false;
            try
            {
                await SaveSettingsAsync();
                await RefreshTargetsAsync();
            }
            finally
            {
                RefreshButton.IsEnabled = true;
                _suppressSelection = false;
            }
        }

        private async Task SaveSettingsAsync()
        {
            _settings.BaseUrl = BaseUrlBox.Text.Trim();
            _settings.Username = UsernameBox.Text.Trim();
            _settings.Password = PasswordBox.Password;
            // 原样保存用户输入；相对路径（如 ./sunshine.exe）在使用时以本程序所在目录解析，
            // 因此随包目录整体挪动仍然有效，保存成绝对路径反而会让包失去便携性。
            _settings.SunshineExePath = SunshinePathBox.Text.Trim();
            _settings.Save();
            await Task.Yield();
        }

        /// <summary>
        /// 编辑框失去焦点时立即记录 sunshine.exe 路径，避免重启后回退为默认值。
        /// </summary>
        private async void SunshinePathBox_LostFocus(object sender, RoutedEventArgs e)
        {
            await SaveSettingsAsync();
        }

        private async void LaunchButton_Click(object sender, RoutedEventArgs e)
        {
            await LaunchSunshineAsync();
        }

        private async Task LaunchSunshineAsync()
        {
            SetStatus("检查 Sunshine...");

            var service = CreateService();
            var exePath = SunshinePathBox.Text.Trim();

            if (SunshineService.IsRunning())
            {
                SetStatus("Sunshine 已在运行。");
            }
            else if (!SunshineService.StartIfNeeded(exePath))
            {
                SetStatus("启动 Sunshine 失败，请检查路径。");
                return;
            }
            else
            {
                SetStatus("已启动 Sunshine，等待 API 就绪...");
            }

            // 启动成功即记录解析后的绝对路径。
            await SaveSettingsAsync();

            if (await service.WaitReadyAsync(TimeSpan.FromSeconds(15)))
            {
                SetStatus("Sunshine API 已就绪。");
            }
            else
            {
                SetStatus("Sunshine 已启动但 API 未响应。");
            }

            service.Dispose();
        }

        /// <summary>
        /// "打开 Sunshine 主页"按钮（设置页与录制目标页各一处，共用本处理器）。
        /// </summary>
        /// <param name="sender">事件源。</param>
        /// <param name="e">事件参数。</param>
        private void OpenWebButton_Click(object sender, RoutedEventArgs e)
        {
            OpenSunshineWeb();
        }

        /// <summary>
        /// 录制目标页的"PIN"按钮：打开 Sunshine Web UI 的 PIN 页。
        /// </summary>
        /// <param name="sender">事件源。</param>
        /// <param name="e">事件参数。</param>
        private void OpenPinButton_Click(object sender, RoutedEventArgs e)
        {
            OpenSunshineWeb(SunshinePinPath);
        }

        /// <summary>
        /// 在默认浏览器中打开 Sunshine Web UI（主页或 PIN 页）。Sunshine 未运行时弹窗提示。
        /// </summary>
        /// <param name="relativePath">相对 Web UI 根的路径（如 /pin）；为空表示打开主页。</param>
        private void OpenSunshineWeb(string relativePath = null)
        {
            if (!SunshineService.IsRunning())
            {
                MessageBox.Show(this, "未检测到 Sunshine 正在运行，请先启动 Sunshine。",
                    "Sunshine 窗口控制器", MessageBoxButton.OK, MessageBoxImage.Information);
                return;
            }

            var baseUrl = BaseUrlBox.Text.Trim();
            if (!Uri.TryCreate(baseUrl, UriKind.Absolute, out var baseUri))
            {
                MessageBox.Show(this, "Web UI 地址无效：" + baseUrl,
                    "Sunshine 窗口控制器", MessageBoxButton.OK, MessageBoxImage.Warning);
                return;
            }

            var target = string.IsNullOrEmpty(relativePath) ? baseUri : new Uri(baseUri, relativePath);

            try
            {
                Process.Start(new ProcessStartInfo(target.ToString()) { UseShellExecute = true });
                SetStatus("已在浏览器中打开：" + target);
            }
            catch (Exception ex)
            {
                MessageBox.Show(this, "打开 Sunshine 主页失败：" + ex.Message,
                    "Sunshine 窗口控制器", MessageBoxButton.OK, MessageBoxImage.Warning);
            }
        }

        private async Task RefreshTargetsAsync()
        {
            if (_isRefreshing)
                return;
            _isRefreshing = true;
            var generation = ++_refreshGeneration;

            try
            {
                var spoofSnapshot = new System.Collections.Generic.Dictionary<uint, bool>(_hookStateByPid);
                var result = await Task.Run(() =>
                {
                    var items = new System.Collections.Generic.List<CaptureTarget>();

                    // 桌面条目
                    items.Add(new CaptureTarget
                    {
                        Title = "桌面",
                        Hwnd = IntPtr.Zero,
                        Pid = 0,
                        Subtitle = "全屏桌面录制",
                        Thumbnail = ThumbnailCapture.CaptureDesktop(),
                    });

                    var infoList = WindowEnumerator.GetVisibleWindows();
                    foreach (var w in infoList)
                    {
                        var thumb = ThumbnailCapture.CaptureWindow(w.Hwnd) ??
                                    ThumbnailCapture.CreatePlaceholder(w.Title);
                        bool spoofOn = spoofSnapshot.TryGetValue(w.Pid, out bool hooked) && hooked;
                        items.Add(new CaptureTarget
                        {
                            Title = w.Title,
                            Hwnd = w.Hwnd,
                            Pid = w.Pid,
                            Subtitle = "PID " + w.Pid,
                            Thumbnail = thumb,
                            IsFocusSpoofOn = spoofOn,
                        });
                    }
                    return items;
                });

                // 重建列表前记住当前选中项，重建后恢复选中（不触发切换录制目标）。
                var selected = TargetList.SelectedItem as CaptureTarget;

                Targets.Clear();
                foreach (var item in result)
                {
                    Targets.Add(item);
                }

                if (selected != null)
                {
                    _suppressSelection = true;
                    CaptureTarget restored = selected.IsDesktop
                        ? result.FirstOrDefault(t => t.IsDesktop)
                        : result.FirstOrDefault(t => t.Pid == selected.Pid && t.Hwnd == selected.Hwnd)
                          ?? result.FirstOrDefault(t => t.Pid == selected.Pid);
                    if (restored != null)
                        TargetList.SelectedItem = restored;
                    _suppressSelection = false;
                }

                TargetCountText.Text = "共找到 " + (result.Count - 1) + " 个窗口，加上桌面。";
            }
            catch (Exception ex)
            {
                SetStatus("刷新失败：" + ex.Message);
            }
            finally
            {
                _isRefreshing = false;
            }
        }

        private async void TargetList_SelectionChanged(object sender, SelectionChangedEventArgs e)
        {
            if (_suppressSelection)
                return;
            if (e.AddedItems.Count == 0)
                return;

            var target = e.AddedItems[0] as CaptureTarget;
            if (target == null)
                return;

            await SwitchTargetAsync(target);
        }

        private async void TargetList_PreviewMouseLeftButtonDown(object sender, System.Windows.Input.MouseButtonEventArgs e)
        {
            // 重复点击已选中项时触发
            if (_suppressSelection)
                return;

            // 点击每行右侧的「开启/关闭窗口伪聚焦」按钮时不重新切换录制目标
            if (FindAncestor<Button>(e.OriginalSource as DependencyObject) != null)
                return;

            var item = TargetList.ContainerFromElement(e.OriginalSource as DependencyObject) as ListBoxItem;
            if (item == null || !item.IsSelected)
                return;

            var target = item.DataContext as CaptureTarget;
            if (target == null)
                return;

            await SwitchTargetAsync(target);
        }

        /// <summary>
        /// 在可视树上向上查找指定类型的祖先元素，找不到返回 null。
        /// </summary>
        private static T FindAncestor<T>(DependencyObject node) where T : DependencyObject
        {
            while (node != null)
            {
                if (node is T match)
                    return match;

                node = node is System.Windows.Media.Visual || node is System.Windows.Media.Media3D.Visual3D
                    ? System.Windows.Media.VisualTreeHelper.GetParent(node)
                    : System.Windows.LogicalTreeHelper.GetParent(node);
            }
            return null;
        }

        private async Task SwitchTargetAsync(CaptureTarget target)
        {
            var service = CreateService();

            if (!SunshineService.IsRunning())
            {
                SetStatus("Sunshine 未运行，正在启动...");
                var exePath = SunshinePathBox.Text.Trim();
                if (!SunshineService.StartIfNeeded(exePath))
                {
                    SetStatus("启动 Sunshine 失败，请检查路径。");
                    service.Dispose();
                    return;
                }

                var ready = await service.WaitReadyAsync(TimeSpan.FromSeconds(15));
                if (!ready)
                {
                    SetStatus("Sunshine 已启动但 API 未就绪。");
                    service.Dispose();
                    return;
                }
            }

            // 本次用到的 sunshine.exe 路径要持久化（已解析为绝对路径）。
            await SaveSettingsAsync();

            SetStatus("切换到：" + target.Title + (target.IsDesktop ? "" : " (PID " + target.Pid + ")"));

            try
            {
                // 桌面模式回到全屏桌面：显式关闭 XInput-over-UDP 改写，让虚拟手柄
                // 恢复为系统唯一的输入通路；仅游戏目标才携带进程改写开关。
                var xinputDelivery = target.IsDesktop ? "off" : BuildXinputDeliveryValue();
                var xinputPort = _settings.XInputPort;
                var status = target.IsDesktop
                    ? await service.SwitchToWindowAsync(IntPtr.Zero, xinputDelivery, xinputPort)
                    : await service.SwitchToPidAsync(target.Pid, xinputDelivery, xinputPort);

                switch ((int)status)
                {
                    case 200:
                        // 仅切换录制目标；是否注入焦点伪造由每行的「开启窗口伪聚焦」按钮决定。
                        SetStatus("已切换到：" + target.Title + (target.IsDesktop ? "" : " (PID " + target.Pid + ")"));
                        break;
                    case 400:
                        SetStatus("请求失败：无法解析目标窗口。");
                        break;
                    case 401:
                        SetStatus("认证失败，请检查 Web UI 用户名/密码。");
                        break;
                    case 403:
                        SetStatus("禁止访问：此地址的远程访问被阻止。");
                        break;
                    default:
                        SetStatus("Sunshine 返回 HTTP " + (int)status + "。");
                        break;
                }
            }
            catch (Exception ex)
            {
                SetStatus("请求失败：" + ex.Message);
            }
            finally
            {
                service.Dispose();
            }
        }

        private void SetStatus(string message)
        {
            if (!Dispatcher.CheckAccess())
            {
                Dispatcher.Invoke(() => StatusText.Text = message);
            }
            else
            {
                StatusText.Text = message;
            }
        }

        /// <summary>
        /// 列表每一行右侧的「开启窗口伪聚焦」/「关闭窗口伪聚焦」按钮。
        /// 未注入过：首次点击注入 DLL；已注入且开启：点击调用 SpoofStop 卸载 hook；
        /// 已注入但已卸载：点击调用 SpoofStart 重新启用（不重复注入，因为 DLL 已加载）。
        /// </summary>
        private async void FocusSpoofButton_Click(object sender, RoutedEventArgs e)
        {
            var button = sender as Button;
            var target = button?.DataContext as CaptureTarget;
            if (target == null || target.IsDesktop)
                return;

            button.IsEnabled = false;
            _spoofBusy = true;
            try
            {
                // 记录本次使用的 sunshine.exe 路径。
                await SaveSettingsAsync();

                if (target.IsFocusSpoofOn)
                {
                    SetStatus("正在关闭窗口伪聚焦：" + target.Title + " ...");
                    var ok = await Task.Run(() => FocusSpoof.CallExport(target.Pid, "SpoofStop", out var msg)
                        ? (true, msg)
                        : (false, msg));
                    SetStatus(ok.Item2);
                    if (ok.Item1)
                    {
                        target.IsFocusSpoofOn = false;
                        _hookStateByPid[target.Pid] = false;
                    }
                }
                else if (_hookStateByPid.ContainsKey(target.Pid))
                {
                    // 之前已注入过、现在处于「已卸载」状态：调用导出函数重新启用，
                    // 而不是再次注入（DLL 已加载，再次 LoadLibrary 不会重跑 DllMain）。
                    SetStatus("正在开启窗口伪聚焦：" + target.Title + " ...");
                    var ok = await Task.Run(() => FocusSpoof.CallExport(target.Pid, "SpoofStart", out var msg)
                        ? (true, msg)
                        : (false, msg));
                    SetStatus(ok.Item2);
                    if (ok.Item1)
                    {
                        target.IsFocusSpoofOn = true;
                        _hookStateByPid[target.Pid] = true;
                    }
                }
                else
                {
                    SetStatus("正在开启窗口伪聚焦：" + target.Title + " ...");
                    var focusOk = await Task.Run(() => FocusSpoof.SpoofActivation(target.Pid, target.Hwnd, out string msg)
                        ? (true, msg)
                        : (false, msg));
                    SetStatus(focusOk.Item2);
                    if (focusOk.Item1)
                    {
                        target.IsFocusSpoofOn = true;
                        _hookStateByPid[target.Pid] = true;
                    }
                }
            }
            finally
            {
                button.IsEnabled = true;
                _spoofBusy = false;
            }
        }

        /// <summary>
        /// 将当前所有焦点伪造选项写入配置文件（focus_options.txt + SunshineWindowController.json）。
        /// </summary>
        private async void SaveConfigButton_Click(object sender, RoutedEventArgs e)
        {
            WriteAllFocusOptions();

            _settings.SuspendThreadsOnPatch = SuspendOnPatchCheck.IsChecked == true;
            _settings.EnableSubclass = EnableSubclassCheck.IsChecked == true;
            _settings.BlockPollingApis = BlockPollApisCheck.IsChecked == true;
            _settings.BlockWmInput = BlockWmInputCheck.IsChecked == true;
            _settings.BlockRawInputApis = BlockRawApisCheck.IsChecked == true;
            _settings.BlockCursorHide = BlockCursorHideCheck.IsChecked == true;
            _settings.BlockCursorLock = BlockCursorLockCheck.IsChecked == true;
            _settings.DisableAll = DisableAllCheck.IsChecked == true;
            _settings.XInputEnabled = XInputEnabledCheck.IsChecked == true;
            _settings.XInputPort = XInputPortCombo.SelectedItem is int port ? port : _settings.XInputPort;

            await SaveSettingsAsync();
            SetStatus("配置已保存到：" + FocusSpoof.ConfigPath);
        }

        /// <summary>
        /// 从配置文件重新加载所有焦点伪造选项到界面。
        /// </summary>
        private void LoadConfigButton_Click(object sender, RoutedEventArgs e)
        {
            LoadFocusOptions();
            SetStatus("配置已重新加载。");
        }

        /// <summary>
        /// 把界面上的全部焦点伪造选项一次性写入 focus_options.txt。
        /// </summary>
        private void WriteAllFocusOptions()
        {
            FocusSpoof.SetOption("SuspendThreadsOnPatch", SuspendOnPatchCheck.IsChecked == true ? 1 : 0);
            FocusSpoof.SetOption("EnableSubclass", EnableSubclassCheck.IsChecked == true ? 1 : 0);
            FocusSpoof.SetOption("BlockPollingApis", BlockPollApisCheck.IsChecked == true ? 1 : 0);
            FocusSpoof.SetOption("BlockWmInput", BlockWmInputCheck.IsChecked == true ? 1 : 0);
            FocusSpoof.SetOption("BlockRawInputApis", BlockRawApisCheck.IsChecked == true ? 1 : 0);
            FocusSpoof.SetOption("BlockCursorHide", BlockCursorHideCheck.IsChecked == true ? 1 : 0);
            FocusSpoof.SetOption("BlockCursorLock", BlockCursorLockCheck.IsChecked == true ? 1 : 0);
            FocusSpoof.SetOption("DisableFocusSpoof", DisableAllCheck.IsChecked == true ? 1 : 0);
            FocusSpoof.SetOption("XInputRewrite", XInputEnabledCheck.IsChecked == true ? 1 : 0);
            FocusSpoof.SetOption("XInputUdpPort", XInputPortCombo.SelectedItem is int port ? port : _settings.XInputPort);
        }

        /// <summary>
        /// 下拉框改变 XInput UDP 端口时立即生效：写入 focus_options.txt（DLL 侧读取）
        /// 并同步到 AppSettings 持久化，之后的游戏注入与 /api/capture-window 请求都会
        /// 使用新端口。初始化期间（_settings 尚未加载）本事件跳过后在加载时统一应用。
        /// </summary>
        private void XInputPortCombo_SelectionChanged(object sender, SelectionChangedEventArgs e)
        {
            if (XInputPortCombo.SelectedItem is int port && _settings != null && _isLoaded)
            {
                _settings.XInputPort = port;
                FocusSpoof.SetOption("XInputUdpPort", port);
                _settings.Save();
                SetStatus("XInput UDP 端口已切换：" + port);
            }
        }
    }
}
