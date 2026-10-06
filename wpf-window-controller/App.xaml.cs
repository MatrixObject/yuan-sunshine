using System;
using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;
using System.Windows;

namespace SunshineWindowController
{
    /// <summary>
    /// Application entry point for the Sunshine Window Controller.
    /// </summary>
    public partial class App : Application
    {
        /// <summary>名称互斥体，用于保证单实例运行（Session 级）。</summary>
        private const string SingleInstanceMutexName = @"Local\SunshineWindowController.SingleInstance";

        /// <summary>主窗口标题，供其它实例定位窗口。</summary>
        public const string MainWindowTitle = "Sunshine 窗口控制器";

        /// <summary>其它实例请求显示主窗口的自定义窗口消息名。</summary>
        public const string ShowMainWindowMessageName = "SunshineWindowController.ShowMainWindow";

        private Mutex _singleInstanceMutex;
        private bool _ownsMutex;

        /// <summary>
        /// 单实例入口：后启动的实例把已有实例的主窗口带到前台后立即退出。
        /// </summary>
        /// <param name="e">启动事件参数。</param>
        protected override void OnStartup(StartupEventArgs e)
        {
            _singleInstanceMutex = new Mutex(true, SingleInstanceMutexName, out bool createdNew);
            _ownsMutex = createdNew;
            if (!createdNew)
            {
                ActivateExistingInstance();
                Shutdown();
                return;
            }

            // 清理过期日志：logs 目录下超过 1 天的 focusspoof_target_*.log。
            Services.FocusSpoof.CleanupStaleLogs();

            base.OnStartup(e);
        }

        /// <summary>
        /// 查找已运行实例的主窗口并显示到前台：先发自定义消息让主窗口自己恢复显示
        /// （窗口可能已隐藏到托盘，单纯 SetForegroundWindow 对隐藏窗口无效），再置前。
        /// 窗口隐藏后 Process.MainWindowHandle 会返回 IntPtr.Zero，因此主窗口由
        /// FindMainWindowOfProcess 自行枚举，避免二次启动时找不到目标而什么都不做。
        /// </summary>
        private static void ActivateExistingInstance()
        {
            int currentPid = Process.GetCurrentProcess().Id;
            foreach (var process in Process.GetProcessesByName(Process.GetCurrentProcess().ProcessName))
            {
                if (process.Id == currentPid)
                {
                    process.Dispose();
                    continue;
                }

                try
                {
                    int targetPid = process.Id;
                    IntPtr handle = FindMainWindowOfProcess(targetPid);
                    if (handle == IntPtr.Zero)
                    {
                        process.Refresh();
                        handle = process.MainWindowHandle;
                    }

                    if (handle != IntPtr.Zero)
                    {
                        // 后启动的实例由用户亲手拉起，握有前台权限；转交给已有实例，
                        // 它消息回调里的 Activate() 才能把窗口真正带到前台。
                        AllowSetForegroundWindow(targetPid);

                        var message = RegisterWindowMessage(ShowMainWindowMessageName);
                        if (message != 0)
                            PostMessage(handle, message, IntPtr.Zero, IntPtr.Zero);

                        ShowWindow(handle, SW_RESTORE);
                        SetForegroundWindow(handle);
                    }
                }
                catch (Exception)
                {
                    // 尽力而为。
                }
                finally
                {
                    process.Dispose();
                }

                break;
            }
        }

        /// <summary>
        /// 按进程 ID 枚举顶层窗口，找出它的主窗口。
        ///
        /// 隐藏到托盘的窗口不会被 Process.MainWindowHandle 找到，但依然存在于窗口枚举里，
        /// 所以二次启动时靠这个方法定位，之后再由消息让对方自己 Show()。仅靠"带标题栏且
        /// 没有拥有者"还不够：同一个进程里 WPF 的内部窗口、WinForms 的辅助窗口同样满足，
        /// 因此继续要求窗口有标题，并优先精确匹配 MainWindowTitle。
        /// </summary>
        /// <param name="processId">目标进程 ID。</param>
        /// <returns>主窗口句柄；找不到时返回 IntPtr.Zero。</returns>
        private static IntPtr FindMainWindowOfProcess(int processId)
        {
            IntPtr titled = IntPtr.Zero;
            IntPtr exact = IntPtr.Zero;

            EnumWindows((handle, lParam) =>
            {
                GetWindowThreadProcessId(handle, out uint windowPid);
                if (windowPid != processId)
                    return true;

                if ((GetWindowLong(handle, GWL_STYLE) & WS_CAPTION) == 0)
                    return true;

                if (GetWindow(handle, GW_OWNER) != IntPtr.Zero)
                    return true;

                var buffer = new StringBuilder(256);
                if (GetWindowText(handle, buffer, buffer.Capacity) == 0)
                    return true;

                if (titled == IntPtr.Zero)
                    titled = handle;

                if (string.Equals(buffer.ToString(), MainWindowTitle, StringComparison.Ordinal))
                {
                    exact = handle;
                    return false;
                }

                return true;
            }, IntPtr.Zero);

            if (exact != IntPtr.Zero)
                return exact;

            return titled;
        }

        /// <summary>
        /// 退出时释放单实例互斥体（仅释放自己成功创建的）。
        /// </summary>
        /// <param name="e">退出事件参数。</param>
        protected override void OnExit(ExitEventArgs e)
        {
            if (_ownsMutex && _singleInstanceMutex != null)
            {
                _singleInstanceMutex.ReleaseMutex();
                _singleInstanceMutex.Dispose();
                _ownsMutex = false;
            }

            base.OnExit(e);
        }

        /// <summary>ShowWindow 的 SW_RESTORE。</summary>
        private const int SW_RESTORE = 9;

        /// <summary>GetWindowLong 取窗口样式时使用的索引。</summary>
        private const int GWL_STYLE = -16;

        /// <summary>GetWindow 取窗口拥有者时使用的索引。</summary>
        private const int GW_OWNER = 4;

        /// <summary>窗口样式中表示"带标题栏"的位。</summary>
        private const int WS_CAPTION = 0x00C00000;

        /// <summary>
        /// EnumWindows 的回调。
        /// </summary>
        /// <param name="handle">正在考察的窗口句柄。</param>
        /// <param name="lParam">枚举附带的参数。</param>
        /// <returns>希望继续枚举返回 True，返回 False 则中止。</returns>
        private delegate bool EnumWindowsProc(IntPtr handle, IntPtr lParam);

        /// <summary>
        /// 枚举所有顶层窗口。
        /// </summary>
        /// <param name="lpEnumFunc">逐个窗口考察的回调。</param>
        /// <param name="lParam">传给回调的参数。</param>
        /// <returns>成功时返回 True。</returns>
        [DllImport("user32.dll")]
        private static extern bool EnumWindows(EnumWindowsProc lpEnumFunc, IntPtr lParam);

        /// <summary>
        /// 取窗口所属的进程 ID。
        /// </summary>
        /// <param name="handle">窗口句柄。</param>
        /// <param name="processId">接收进程 ID 的变量。</param>
        /// <returns>窗口所属线程的 ID。</returns>
        [DllImport("user32.dll")]
        private static extern uint GetWindowThreadProcessId(IntPtr handle, out uint processId);

        /// <summary>
        /// 取窗口的样式位。
        /// </summary>
        /// <param name="handle">窗口句柄。</param>
        /// <param name="nIndex">样式索引（GWL_STYLE）。</param>
        /// <returns>样式位；失败时返回 0。</returns>
        [DllImport("user32.dll", CharSet = CharSet.Unicode)]
        private static extern int GetWindowLong(IntPtr handle, int nIndex);

        /// <summary>
        /// 取窗口的拥有者窗口。
        /// </summary>
        /// <param name="handle">窗口句柄。</param>
        /// <param name="uCmd">关系索引（GW_OWNER）。</param>
        /// <returns>拥有者窗口句柄；没有时返回 IntPtr.Zero。</returns>
        [DllImport("user32.dll")]
        private static extern IntPtr GetWindow(IntPtr handle, int uCmd);

        /// <summary>
        /// 取窗口标题文字。
        /// </summary>
        /// <param name="handle">窗口句柄。</param>
        /// <param name="buffer">接收标题的缓冲区。</param>
        /// <param name="maxCount">缓冲区容量。</param>
        /// <returns>复制到缓冲区的字符数；窗口没有标题时返回 0。</returns>
        [DllImport("user32.dll", CharSet = CharSet.Unicode)]
        private static extern int GetWindowText(IntPtr handle, StringBuilder buffer, int maxCount);

        /// <summary>
        /// 允许指定进程把窗口切到前台。
        /// </summary>
        /// <param name="dwProcessId">被授权的进程 ID。</param>
        /// <returns>授权成功返回 True。</returns>
        [DllImport("user32.dll")]
        private static extern bool AllowSetForegroundWindow(int dwProcessId);

        /// <summary>
        /// 将指定窗口带到前台。
        /// </summary>
        /// <param name="handle">窗口句柄。</param>
        /// <returns>成功时返回 True。</returns>
        [DllImport("user32.dll")]
        private static extern bool SetForegroundWindow(IntPtr handle);

        /// <summary>
        /// 显示/恢复指定窗口。
        /// </summary>
        /// <param name="handle">窗口句柄。</param>
        /// <param name="cmd">显示命令。</param>
        /// <returns>成功时返回 True。</returns>
        [DllImport("user32.dll")]
        private static extern bool ShowWindow(IntPtr handle, int cmd);

        /// <summary>
        /// 注册自定义窗口消息。
        /// </summary>
        /// <param name="message">消息名。</param>
        /// <returns>消息号，失败返回 0。</returns>
        [DllImport("user32.dll", CharSet = CharSet.Unicode)]
        private static extern uint RegisterWindowMessage(string message);

        /// <summary>
        /// 向窗口发送消息。
        /// </summary>
        /// <param name="handle">窗口句柄。</param>
        /// <param name="msg">消息号。</param>
        /// <param name="wParam">消息参数。</param>
        /// <param name="lParam">消息参数。</param>
        /// <returns>成功时返回 True。</returns>
        [DllImport("user32.dll")]
        private static extern bool PostMessage(IntPtr handle, uint msg, IntPtr wParam, IntPtr lParam);
    }
}