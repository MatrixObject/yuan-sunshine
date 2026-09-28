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

        private Mutex _singleInstanceMutex;

        /// <summary>
        /// 单实例入口：后启动的实例把已有实例的主窗口带到前台后立即退出。
        /// </summary>
        /// <param name="e">启动事件参数。</param>
        protected override void OnStartup(StartupEventArgs e)
        {
            _singleInstanceMutex = new Mutex(true, SingleInstanceMutexName, out bool createdNew);
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
        /// 查找已运行实例的主窗口并带到前台（尽力而为，失败静默忽略）。
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
                        SetForegroundWindow(handle);
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
        /// 退出时释放单实例互斥体。
        /// </summary>
        /// <param name="e">退出事件参数。</param>
        protected override void OnExit(ExitEventArgs e)
        {
            _singleInstanceMutex?.ReleaseMutex();
            _singleInstanceMutex?.Dispose();
            base.OnExit(e);
        }

        /// <summary>
        /// 将指定窗口带到前台。
        /// </summary>
        /// <param name="handle">窗口句柄。</param>
        /// <returns>成功时返回 True。</returns>
        [DllImport("user32.dll")]
        private static extern bool SetForegroundWindow(IntPtr handle);
    }
}