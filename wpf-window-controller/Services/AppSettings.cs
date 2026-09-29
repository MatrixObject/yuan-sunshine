using System;
using System.IO;
using Newtonsoft.Json;

namespace SunshineWindowController.Services
{
    /// <summary>
    /// 应用程序设置，持久化到 JSON 文件。
    /// </summary>
    public sealed class AppSettings
    {
        /// <summary>Sunshine Web UI 地址，默认 https://localhost:47990</summary>
        public string BaseUrl { get; set; } = "https://localhost:47990";

        /// <summary>Web UI 用户名</summary>
        public string Username { get; set; } = "sunshine";

        /// <summary>Web UI 密码</summary>
        public string Password { get; set; } = "";

        /// <summary>sunshine.exe 路径，默认相对路径 ./sunshine.exe（即本程序所在目录）</summary>
        public string SunshineExePath { get; set; } = "./sunshine.exe";

        /// <summary>录制时保持游戏窗口激活（伪聚焦），默认开启</summary>
        public bool KeepFocused { get; set; } = true;

        // ===== 焦点伪造选项 =====

        /// <summary>打补丁时冻结线程，默认开启</summary>
        public bool SuspendThreadsOnPatch { get; set; } = true;

        /// <summary>开启窗口子类化（用于伪聚焦和 WM_INPUT 屏蔽），默认开启</summary>
        public bool EnableSubclass { get; set; } = true;

        /// <summary>屏蔽键鼠状态 API（GetKeyState / GetAsyncKeyState / GetKeyboardState），默认开启</summary>
        public bool BlockPollingApis { get; set; } = true;

        /// <summary>屏蔽 WM_INPUT（需要开启窗口子类化），默认开启</summary>
        public bool BlockWmInput { get; set; } = true;

        /// <summary>屏蔽 RawInput 相关 API（GetRawInputData/GetRawInputBuffer，与 WM_INPUT 功能相同但不需要子类化），默认开启</summary>
        public bool BlockRawInputApis { get; set; } = true;

        /// <summary>ShowCursor 直通控制点（拦截隐藏会使游戏显示计数失同步而死循环），默认开启</summary>
        public bool BlockCursorHide { get; set; } = true;

        /// <summary>阻止游戏锁定鼠标光标（拒绝 ClipCursor / GetClipCursor 报告全屏），默认开启</summary>
        public bool BlockCursorLock { get; set; } = true;

        /// <summary>禁用所有焦点伪造（仅加载 DLL）</summary>
        public bool DisableAll { get; set; } = false;

        // ===== XInput 手柄改写选项 =====

        /// <summary>把流会话的手柄状态 UDP 推给被注入的游戏进程（仅针对该进程改写其 XInputGetState），默认开启</summary>
        public bool XInputEnabled { get; set; } = true;

        /// <summary>XInput UDP 固定端口的 5 个可选预设（下拉框选项），需与 DLL 侧 XInputUdpPort 保持一致。</summary>
        public static readonly int[] XInputPortPresets = { 45680, 45690, 45700, 45710, 45720 };

        /// <summary>XInput UDP 固定端口，DLL 侧 focus_options.txt 的 XInputUdpPort 与此一致，默认 45690</summary>
        public int XInputPort { get; set; } = 45690;

        /// <summary>
        /// 设置文件路径：本程序所在目录下的 SunshineWindowController.json（便携式，
        /// 不写 %APPDATA%，随包目录整体挪动/复制时配置不丢失、不串机器）。
        /// 与包内 sunshine.exe 同目录，文件名带控制器全名以区分归属。
        /// </summary>
        private static string SettingsPath =>
            Path.Combine(AppDomain.CurrentDomain.BaseDirectory, "SunshineWindowController.json");

        /// <summary>
        /// 从磁盘加载设置，文件不存在时返回默认值。
        /// </summary>
        public static AppSettings Load()
        {
            try
            {
                var path = SettingsPath;
                if (!File.Exists(path))
                    return new AppSettings();

                var json = File.ReadAllText(path);
                var settings = JsonConvert.DeserializeObject<AppSettings>(json) ?? new AppSettings();
                NormalizeSunshineExePath(settings);
                return settings;
            }
            catch (Exception)
            {
                return new AppSettings();
            }
        }

        /// <summary>
        /// 归一化 sunshine.exe 路径：当本程序所在目录存在 sunshine.exe（便携包场景）时，
        /// 强制使用相对路径 ./sunshine.exe，覆盖历史遗留的绝对路径，保证包随目录挪动仍有效。
        /// </summary>
        /// <param name="settings">加载后的设置。</param>
        private static void NormalizeSunshineExePath(AppSettings settings)
        {
            try
            {
                var local = Path.Combine(AppDomain.CurrentDomain.BaseDirectory, "sunshine.exe");
                if (File.Exists(local))
                    settings.SunshineExePath = "./sunshine.exe";
            }
            catch (Exception)
            {
                // 静默失败，保留原设置
            }
        }

        /// <summary>
        /// 保存设置到磁盘。
        /// </summary>
        public void Save()
        {
            try
            {
                var dir = Path.GetDirectoryName(SettingsPath);
                if (!Directory.Exists(dir))
                    Directory.CreateDirectory(dir);

                var json = JsonConvert.SerializeObject(this, Formatting.Indented);
                File.WriteAllText(SettingsPath, json);
            }
            catch (Exception)
            {
                // 静默失败，设置仍可在内存中工作
            }
        }
    }
}
