using System;
using System.Diagnostics;
using System.Runtime.InteropServices;
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
                    process.Refresh();
                    IntPtr handle = process.MainWindowHandle;
                    if (handle != IntPtr.Zero)
                    {
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