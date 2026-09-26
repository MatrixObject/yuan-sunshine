using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Text;

namespace SunshineWindowController.Services
{
    /// <summary>
    /// Enumerates visible top-level windows on the current desktop,
    /// excluding any window owned by the current process.
    /// </summary>
    internal static class WindowEnumerator
    {
        private const int GWL_EXSTYLE = -20;
        private const long WS_EX_TOOLWINDOW = 0x00000080L;
        private const uint DWMWA_CLOAKED = 14;
        private const uint PROCESS_QUERY_LIMITED_INFORMATION = 0x1000;

        [DllImport("user32.dll")]
        private static extern bool EnumWindows(EnumWindowsProc lpEnumFunc, IntPtr lParam);

        [DllImport("user32.dll")]
        private static extern bool IsWindowVisible(IntPtr hWnd);

        [DllImport("user32.dll")]
        private static extern int GetWindowTextLengthW(IntPtr hWnd);

        [DllImport("user32.dll", CharSet = CharSet.Unicode)]
        private static extern int GetWindowTextW(IntPtr hWnd, StringBuilder lpString, int nMaxCount);

        [DllImport("user32.dll", CharSet = CharSet.Unicode)]
        private static extern int GetClassNameW(IntPtr hWnd, StringBuilder lpClassName, int nMaxCount);

        [DllImport("user32.dll")]
        private static extern uint GetWindowThreadProcessId(IntPtr hWnd, out uint lpdwProcessId);

        [DllImport("user32.dll", EntryPoint = "GetWindowLongPtrW")]
        private static extern IntPtr GetWindowLongPtr(IntPtr hWnd, int nIndex);

        [DllImport("dwmapi.dll")]
        private static extern int DwmGetWindowAttribute(IntPtr hwnd, uint dwAttribute, out int pvAttribute, int cbAttribute);

        [DllImport("kernel32.dll")]
        private static extern IntPtr OpenProcess(uint dwDesiredAccess, bool bInheritHandle, uint dwProcessId);

        [DllImport("kernel32.dll", CharSet = CharSet.Unicode)]
        private static extern bool QueryFullProcessImageNameW(IntPtr hProcess, uint dwFlags, StringBuilder lpExeName, ref uint lpdwSize);

        [DllImport("kernel32.dll")]
        private static extern bool CloseHandle(IntPtr hObject);

        private delegate bool EnumWindowsProc(IntPtr hWnd, IntPtr lParam);

        /// <summary>控制台窗口的窗口类名（pwsh / cmd 等无界面程序）。</summary>
        private const string ConsoleWindowClassName = "ConsoleWindowClass";

        /// <summary>
        /// 非游戏进程名黑名单（不含扩展名，大小写无关）。只要黑名覆盖得到，
        /// 一律排除：浏览器、资源管理器、任务管理器、记事本、画图、Office/WPS、
        /// 各类 IDE 与编辑器、游戏引擎/建模软件、终端、聊天/远程/协作工具、
        /// 游戏平台（Steam/Epic/GOG...）等。保持此清单可随需要扩充。
        /// </summary>
        private static readonly HashSet<string> NonGameProcessNames = new HashSet<string>(StringComparer.OrdinalIgnoreCase)
        {
            // 游戏平台 / 启动器
            "steam", "epicgameslauncher", "epicwebhelper", "goggalaxy", "battle.net",
            "riotclient", "uplay", "ubisoftconnect",
            // 流媒体串流 / 推流 / 直播（本身非游戏）
            "sunshine", "sunshine_console", "palstreaming", "moonlight", "obs64",
            "obs32", "slobs",
            // 浏览器
            "chrome", "msedge", "firefox", "iexplore", "opera", "opera_gx", "brave",
            "vivaldi", "yandex", "qqbrowser", "sogouexplorer", "360se", "360chrome",
            "maxthon", "centbrowser", "waterfox", "palemoon", "seamonkey", "torbrowser",
            // 系统：文件资源管理器 / 任务管理器 / shell / 服务宿主 / 设置等
            "explorer", "taskmgr", "dwm", "sihost", "dllhost", "svchost", "searchapp",
            "searchhost", "runtimebroker", "startmenuexperiencehost", "systemsettings",
            "shellExperienceHost", "calculator", "control", "msiexec", "dcomcnfg",
            // 远程桌面 / 远程控制 / VNC
            "mstsc", "vncviewer", "nomachine", "rdpclip", "rdpinput",
            "sunloginclient", "sunlogin_org", "todesk", "anydesk", "rustdesk", "teamviewer",
            // 记事本 / 画图 / 截图 / 计算器
            "notepad", "notepad++", "wordpad", "mspaint", "snippingtool",
            // Office
            "winword", "excel", "powerpnt", "outlook", "onenote", "onenoteim", "msaccess",
            // WPS Office
            "wps", "wpscloudsvr", "wpscenter", "wpscfg", "et", "wpp", "wpspdf", "ksolaunch", "wpsstore",
            // LibreOffice
            "libreoffice", "soffice",
            // 终端 / 控制台宿主（无窗口，含 pwsh）。
            // 注意：不放解释器运行时（java / node / python / bash / wsl 等）——
            // java -jar game.jar、python 游戏等由它们托管的程序可能本身就是游戏，
            // 无界面控制台窗口已由上方的 ConsoleWindowClass 检查统一排除。
            "cmd", "conhost", "windowsterminal", "powershell", "pwsh", "powershell_ise",
            // 编辑器
            "code", "cursor", "opencode", "sublime_text", "atom", "zed",
            // IDE（Visual Studio 系列）
            "devenv", "vswinexpress", "wdexpress",
            // IDE（JetBrains 系列）
            "idea", "idea64", "pycharm", "pycharm64", "webstorm", "webstorm64",
            "phpstorm", "phpstorm64", "rubymine", "rubymine64", "goland", "goland64",
            "datagrip", "datagrip64", "clion", "clion64", "rider", "rider64",
            "appcode", "appcode64", "eclipse", "netbeans",
            // IDE（Android Studio 系列）
            "studio", "studio64", "androidstudio", "androidstudio64",
            // 数据库开发工具
            "ssms", "dbeaver", "mysqlworkbench", "navicat",
            // 游戏引擎 / DCC / 建模 / 材质
            "unity", "unityhub", "cocoscreator", "godot", "3dsmax", "blender",
            "maya", "acad", "autodesk",
            // 图形 / 影音处理
            "photoshop", "illustrator", "indesign", "lightroom", "premiere",
            "afterfx", "gimp", "krita", "clipstudiopaint", "audacity",
            // PDF / 文档阅读
            "acrobat", "acroread", "foxitreader", "sumatrapdf",
            // 笔记 / 知识库
            "obsidian", "typora", "notion", "evernote",
            // Git / 代码托管 / API
            "github", "sourcetree", "gitkraken", "postman", "docker",
            // 逆向 / 调试 / 系统工具
            "x64dbg", "x32dbg", "ida", "ida64", "ghidra", "procmon", "procexp", "procexp64", "cheatengine",
            // 聊天 / 办公协作 / 远程控制
            "wechat", "qq", "tim", "dingtalk", "feishu", "lark", "teams", "slack",
            "discord", "telegram", "whatsapp", "zoom",
            // 媒体播放 / 音乐
            "vlc", "potplayer", "wmplayer", "kodi", "qqmusic", "neteasecloudmusic", "foobar2000", "spotify",
            // 压缩 / 下载 / 其它常用
            "7zfm", "winrar", "winscp", "filezilla", "idman",
        };

        /// <summary>
        /// Enumerate all visible, titled, non-cloaked top-level windows,
        /// excluding any window owned by the current process and windows
        /// whose owning process is clearly not a game.
        /// </summary>
        /// <returns>List of window handles with metadata.</returns>
        public static List<WindowInfo> GetVisibleWindows()
        {
            var result = new List<WindowInfo>();

            using (var self = Process.GetCurrentProcess())
            {
                var selfPid = (uint)self.Id;

                EnumWindows((hWnd, lParam) =>
                {
                    if (!IsWindowVisible(hWnd))
                        return true;

                    GetWindowThreadProcessId(hWnd, out var pid);
                    if (pid == selfPid)
                        return true;

                    // 排除非游戏应用（浏览器、资源管理器、office、画图、IDE、
                    // 游戏引擎/建模、终端、聊天/远程、Steam/Epic 等平台...）。
                    // 解析不到进程名（无权访问/进程已退出/无所有者）也一并排除：
                    // 连进程都无法查询到，注入 DLL 基本不可能成功。
                    var processName = GetProcessName(pid);
                    if (processName == null || NonGameProcessNames.Contains(processName))
                        return true;

                    var titleLength = GetWindowTextLengthW(hWnd);
                    if (titleLength <= 0)
                        return true;

                    // 排除无界面的控制台窗口（pwsh / cmd / node / python 等）。
                    var windowClass = new StringBuilder(256);
                    if (GetClassNameW(hWnd, windowClass, windowClass.Capacity) > 0 &&
                        string.Equals(windowClass.ToString(), ConsoleWindowClassName, StringComparison.OrdinalIgnoreCase))
                    {
                        return true;
                    }

                    // Skip tool windows (e.g. tooltips, tray popups).
                    var exStyle = GetWindowLongPtr(hWnd, GWL_EXSTYLE).ToInt64();
                    if ((exStyle & WS_EX_TOOLWINDOW) != 0)
                        return true;

                    // Skip windows cloaked by DWM (e.g. virtual desktops hiding them).
                    if (DwmGetWindowAttribute(hWnd, DWMWA_CLOAKED, out int cloaked, sizeof(int)) == 0 && cloaked != 0)
                        return true;

                    var sb = new StringBuilder(titleLength + 1);
                    GetWindowTextW(hWnd, sb, sb.Capacity);

                    result.Add(new WindowInfo
                    {
                        Hwnd = hWnd,
                        Pid = pid,
                        Title = sb.ToString(),
                    });

                    return true;
                }, IntPtr.Zero);
            }

            return result;
        }

        /// <summary>
        /// Resolve the image file name (without extension) of the process with the given
        /// id, or null when the process has no owner, no longer exists or cannot be
        /// inspected (e.g. access denied). Returns null instead of throwing so
        /// enumeration never fails on a dying or protected process.
        /// </summary>
        private static string GetProcessName(uint pid)
        {
            if (pid == 0)
                return null;

            var handle = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, false, pid);
            if (handle == IntPtr.Zero)
                return null;

            try
            {
                var sb = new StringBuilder(1024);
                uint size = (uint)sb.Capacity;
                if (QueryFullProcessImageNameW(handle, 0, sb, ref size) && size > 0)
                {
                    return System.IO.Path.GetFileNameWithoutExtension(sb.ToString());
                }
            }
            finally
            {
                CloseHandle(handle);
            }

            return null;
        }
    }

    /// <summary>
    /// Metadata for a single top-level window.
    /// </summary>
    internal sealed class WindowInfo
    {
        public IntPtr Hwnd { get; set; }
        public uint Pid { get; set; }
        public string Title { get; set; }
    }
}