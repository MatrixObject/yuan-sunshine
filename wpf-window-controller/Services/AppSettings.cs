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

        /// <summary>屏蔽键鼠输入（XInput 手柄始终有效），默认关闭</summary>
        public bool BlockKeyboardMouse { get; set; } = false;

        // ===== 焦点伪造选项 =====

        /// <summary>打补丁时冻结线程，默认开启</summary>
        public bool SuspendThreadsOnPatch { get; set; } = true;

        /// <summary>启用窗口子类化（旧方式，大多数游戏建议关闭）</summary>
        public bool EnableSubclass { get; set; } = true;

        /// <summary>阻止游戏隐藏鼠标光标（Hook ShowCursor），默认开启</summary>
        public bool BlockCursorHide { get; set; } = true;

        /// <summary>阻止游戏锁定鼠标光标（Hook ClipCursor/GetClipCursor），默认开启</summary>
        public bool BlockCursorLock { get; set; } = true;

        /// <summary>GetRawInputData/GetRawInputBuffer 钩子是键鼠屏蔽的主通道（按 SDK 语义返回 0），默认开启</summary>
        public bool DisableRawInput { get; set; } = false;

        /// <summary>窗口过程层（子类化吞 WM_INPUT），re9 上会闪退，默认关闭</summary>
        public bool BlockLegacyMessages { get; set; } = false;

        /// <summary>禁用所有焦点伪造（仅加载 DLL）</summary>
        public bool DisableAll { get; set; } = false;

        private static string SettingsPath =>
            Path.Combine(
                Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData),
                "SunshineWindowController",
                "settings.json");

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
                return JsonConvert.DeserializeObject<AppSettings>(json) ?? new AppSettings();
            }
            catch (Exception)
            {
                return new AppSettings();
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
