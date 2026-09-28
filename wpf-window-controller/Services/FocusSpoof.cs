using System;
using System.Diagnostics;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;

namespace SunshineWindowController.Services
{
    /// <summary>
    /// 焦点伪造服务：将 focusspoof.dll 注入到游戏进程，使游戏"以为"自己处于前台，
    /// 从而在失焦时仍能保持完整渲染帧率和 XInput 手柄输入响应。
    ///
    /// 注入器（injectedll.exe）由本类启动，DLL 行为受配置文件控制。
    /// 配置文件路径：%APPDATA%\SunshineWindowController\focus_options.txt
    /// 格式：每行一个键值对，如 "BlockPollingApis=1"
    /// </summary>
    internal static class FocusSpoof
    {
        /// <summary>配置文件路径</summary>
        public static string ConfigPath =>
            Path.Combine(
                Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData),
                "SunshineWindowController",
                "focus_options.txt");

        /// <summary>
        /// 清理过期的 focusspoof 目标日志。
        ///
        /// DLL 把诊断日志写到自身旁边的 logs 目录，其中
        /// focusspoof_target_&lt;pid&gt;.log 每次注入都会新增一个，会不断积累。
        /// 控制器启动时只删除名字匹配 focusspoof_target_*.log 且最后写入时间
        /// 超过 1 天的文件（即上次运行遗留的过期日志），其余文件一律不动。
        /// 日志正被仍在运行的注入进程写入时删除会失败，此时静默跳过该文件
        /// （DLL 每次写日志都会重新打开文件，之后仍会正常追加）。
        /// 整个清理是尽力而为的操作，任何失败都不影响启动。
        /// </summary>
        public static void CleanupStaleLogs()
        {
            try
            {
                var dir = Path.Combine(AppDomain.CurrentDomain.BaseDirectory, "logs");
                if (!Directory.Exists(dir))
                    return;

                var staleBefore = DateTime.Now.AddDays(-1);
                foreach (var file in Directory.GetFiles(dir, "focusspoof_target_*.log"))
                {
                    try
                    {
                        if (File.GetLastWriteTime(file) < staleBefore)
                            File.Delete(file);
                    }
                    catch (Exception)
                    {
                        // 文件被占用或无权限，跳过即可。
                    }
                }
            }
            catch (Exception)
            {
                // 日志目录不可访问等异常情况：忽略，不影响启动。
            }
        }

        private const uint PROCESS_QUERY_LIMITED_INFORMATION = 0x1000;
        private const ushort IMAGE_FILE_MACHINE_I386 = 0x014c;

        [DllImport("kernel32.dll")]
        private static extern IntPtr OpenProcess(uint dwDesiredAccess, bool bInheritHandle, uint dwProcessId);

        [DllImport("kernel32.dll")]
        private static extern bool CloseHandle(IntPtr hObject);

        [DllImport("kernel32.dll", EntryPoint = "IsWow64Process2")]
        private static extern bool IsWow64Process2(IntPtr hProcess, out ushort pProcessMachine, out ushort pNativeMachine);

        /// <summary>
        /// 判断目标进程是否为 64 位。注入器的位数必须与目标一致
        /// （WOW64 32 位进程使用自己的一份 kernel32，地址不能跨位宽共用），
        /// 因此需要先探测目标位数来选择 injectedll/injectedll32 与
        /// focusspoof/focusspoof32。
        /// </summary>
        /// <param name="pid">目标进程 ID</param>
        /// <param name="is64">输出的位数判断结果</param>
        /// <param name="message">失败时的原因</param>
        /// <returns>成功判定返回 true</returns>
        private static bool TryGet64Bit(uint pid, out bool is64, out string message)
        {
            message = string.Empty;
            var h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, false, pid);
            if (h == IntPtr.Zero)
            {
                message = "无法打开目标进程（权限不足？），无法判断位数。请以管理员身份运行控制器。";
                is64 = false;
                return false;
            }

            try
            {
                // 32 位系统上不存在 64 位进程。
                if (!Environment.Is64BitOperatingSystem)
                {
                    is64 = false;
                    return true;
                }

                // IsWow64Process2 对 64 位进程返回 0x8664，对 WOW64 进程返回 0x014c。
                if (IsWow64Process2(h, out ushort machine, out _))
                {
                    is64 = machine != IMAGE_FILE_MACHINE_I386;
                    return true;
                }

                // API 不可用（Win10 1511 之前）：若非 WOW64 进程则按 64 位处理。
                is64 = true;
                return true;
            }
            finally
            {
                CloseHandle(h);
            }
        }

        /// <summary>
        /// 按目标进程位数选定注入器与 DLL 的路径。
        /// </summary>
        /// <param name="pid">目标进程 ID</param>
        /// <param name="injector">注入器路径（输出）</param>
        /// <param name="dll">focusspoof DLL 路径（输出）</param>
        /// <param name="message">失败时的原因</param>
        /// <returns>路径就绪返回 true</returns>
        private static bool ResolveBinaries(uint pid, out string injector, out string dll, out string message)
        {
            injector = string.Empty;
            dll = string.Empty;
            if (!TryGet64Bit(pid, out bool is64, out message))
                return false;

            var baseDir = AppDomain.CurrentDomain.BaseDirectory;
            injector = Path.Combine(baseDir, is64 ? "injectedll.exe" : "injectedll32.exe");
            dll = Path.Combine(baseDir, is64 ? "focusspoof.dll" : "focusspoof32.dll");

            if (!File.Exists(injector) || !File.Exists(dll))
            {
                message = "找不到 " + (is64 ? "64 位" : "32 位") + " 注入器或 DLL：" + injector;
                return false;
            }

            return true;
        }

        /// <summary>从配置文件读取开关值</summary>
        public static int GetOption(string name, int def)
        {
            try
            {
                if (!File.Exists(ConfigPath))
                    return def;

                var lines = File.ReadAllLines(ConfigPath);
                foreach (var line in lines)
                {
                    var parts = line.Split(new[] { '=' }, 2);
                    if (parts.Length == 2 && parts[0].Trim() == name)
                    {
                        if (int.TryParse(parts[1].Trim(), out int value))
                            return value;
                    }
                }
                return def;
            }
            catch (Exception)
            {
                return def;
            }
        }

        /// <summary>写入开关值到配置文件</summary>
        public static void SetOption(string name, int value)
        {
            SetOption(name, (long)value);
        }

        /// <summary>写入开关值（64 位）到配置文件</summary>
        public static void SetOption(string name, long value)
        {
            try
            {
                var dir = Path.GetDirectoryName(ConfigPath);
                if (!Directory.Exists(dir))
                    Directory.CreateDirectory(dir);

                var lines = new System.Collections.Generic.List<string>();
                if (File.Exists(ConfigPath))
                {
                    lines.AddRange(File.ReadAllLines(ConfigPath));
                }

                // 更新或添加选项
                bool found = false;
                for (int i = 0; i < lines.Count; i++)
                {
                    var parts = lines[i].Split(new[] { '=' }, 2);
                    if (parts.Length == 2 && parts[0].Trim() == name)
                    {
                        lines[i] = name + "=" + value;
                        found = true;
                        break;
                    }
                }
                if (!found)
                {
                    lines.Add(name + "=" + value);
                }

                File.WriteAllLines(ConfigPath, lines);
            }
            catch (Exception)
            {
                // 静默失败
            }
        }

        /// <summary>
        /// 向游戏进程注入焦点伪造 DLL。
        /// </summary>
        /// <param name="pid">目标游戏进程 ID</param>
        /// <param name="hwnd">目标游戏主窗口句柄（由控制器枚举得到，DLL 锁定此窗口）</param>
        /// <param name="message">状态消息（输出参数）</param>
        /// <returns>注入成功返回 true</returns>
        public static bool SpoofActivation(uint pid, IntPtr hwnd, out string message)
        {
            message = string.Empty;

            if (pid == 0)
            {
                message = "未指定目标进程。";
                return false;
            }

            if (hwnd == IntPtr.Zero)
            {
                message = "未指定目标窗口句柄。";
                return false;
            }

            // 把目标窗口句柄传给 DLL（DLL 优先用外部传入的句柄，不再自找）
            SetOption("TargetWindow", hwnd.ToInt64());

            if (!ResolveBinaries(pid, out var injector, out var dll, out message))
                return false;

            try
            {
                var psi = new ProcessStartInfo
                {
                    FileName = injector,
                    Arguments = pid + " \"" + dll + "\"",
                    UseShellExecute = false,
                    RedirectStandardOutput = true,
                    RedirectStandardError = true,
                    CreateNoWindow = true,
                };

                using var proc = Process.Start(psi);
                if (proc == null)
                {
                    message = "启动注入器失败。";
                    return false;
                }

                var stdout = proc.StandardOutput.ReadToEnd();
                var stderr = proc.StandardError.ReadToEnd();
                proc.WaitForExit(10000);

                if (proc.ExitCode == 0)
                {
                    // LoadLibrary 对已加载的 DLL 不会重跑 DllMain：向同一进程
                    // 重复注入（或停用后再注入）时 attach 逻辑不会执行。这里
                    // 显式调 SpoofStart 重新启用 hook；DLL 侧的 SpoofStart
                    // 每次都会重写 worker 日志，避免跨多次注入累加。
                    if (!CallExport(pid, "SpoofStart", out var startMsg))
                        message = "注入成功，但重新启用失败：" + startMsg;
                    else
                        message = "注入成功：" + stdout.Trim();
                    return true;
                }
                message = "注入失败：" + (stderr.Trim() != "" ? stderr.Trim() : "退出码 " + proc.ExitCode);
                return false;
            }
            catch (Exception ex)
            {
                message = "注入器错误：" + ex.Message;
                return false;
            }
        }

        /// <summary>
        /// 调用已注入 DLL 的导出函数（SpoofStart/SpoofStop），实现"卸载 hook"
        /// 与"重新启用 hook"而不需要重新注入。
        /// </summary>
        /// <param name="pid">目标游戏进程 ID</param>
        /// <param name="exportName">要调用的导出名：SpoofStop 或 SpoofStart</param>
        /// <param name="message">状态消息（输出参数）</param>
        /// <returns>调用成功返回 true</returns>
        public static bool CallExport(uint pid, string exportName, out string message)
        {
            message = string.Empty;

            if (pid == 0)
            {
                message = "未指定目标进程。";
                return false;
            }

            if (exportName != "SpoofStart" && exportName != "SpoofStop")
            {
                message = "未知导出函数：" + exportName;
                return false;
            }

            if (!ResolveBinaries(pid, out var injector, out var dll, out message))
                return false;

            try
            {
                var psi = new ProcessStartInfo
                {
                    FileName = injector,
                    Arguments = "--call " + pid + " \"" + dll + "\" " + exportName,
                    UseShellExecute = false,
                    RedirectStandardOutput = true,
                    RedirectStandardError = true,
                    CreateNoWindow = true,
                };

                using var proc = Process.Start(psi);
                if (proc == null)
                {
                    message = "启动注入器失败。";
                    return false;
                }

                var stdout = proc.StandardOutput.ReadToEnd();
                var stderr = proc.StandardError.ReadToEnd();
                proc.WaitForExit(10000);

                if (proc.ExitCode == 0)
                {
                    message = (exportName == "SpoofStop" ? "已卸载 hook：" : "已重新启用 hook：") + stdout.Trim();
                    return true;
                }
                message = (exportName == "SpoofStop" ? "卸载 hook 失败：" : "启用 hook 失败：")
                          + (stderr.Trim() != "" ? stderr.Trim() : "退出码 " + proc.ExitCode);
                return false;
            }
            catch (Exception ex)
            {
                message = "注入器错误：" + ex.Message;
                return false;
            }
        }
    }
}
