using System;
using System.Diagnostics;
using System.IO;
using System.Net;
using System.Net.Http;
using System.Text;
using System.Threading;
using System.Threading.Tasks;

namespace SunshineWindowController.Services
{
    /// <summary>
    /// Starts Sunshine and talks to its configuration HTTP API.
    /// </summary>
    internal sealed class SunshineService : IDisposable
    {
        private readonly HttpClient _client;

        public SunshineService()
        {
            // Sunshine serves a self-signed certificate; accept it.
            var handler = new HttpClientHandler
            {
                CheckCertificateRevocationList = false,
                ServerCertificateCustomValidationCallback = (_, _, _, _) => true,
                UseDefaultCredentials = false,
            };

            _client = new HttpClient(handler)
            {
                Timeout = TimeSpan.FromSeconds(10),
            };
        }

        /// <summary>
        /// Base URL of the Sunshine Web UI, e.g. https://localhost:47990.
        /// </summary>
        public string BaseUrl { get; set; } = "https://localhost:47990";

        /// <summary>Web UI username.</summary>
        public string Username { get; set; } = "sunshine";

        /// <summary>Web UI password.</summary>
        public string Password { get; set; } = "";

        /// <summary>
        /// 默认的 sunshine.exe 显示路径：相对路径 ./sunshine.exe（相对于本程序所在目录）。
        /// </summary>
        public static string DefaultExePath => "./sunshine.exe";

        /// <summary>
        /// 把用户输入的 sunshine.exe 路径解析为绝对路径：相对路径以本程序所在目录为基准。
        /// </summary>
        /// <param name="exePath">用户输入的可执行文件路径。</param>
        /// <returns>解析后的绝对路径；输入为空时返回 null。</returns>
        public static string ResolveExePath(string exePath)
        {
            if (string.IsNullOrWhiteSpace(exePath))
                return null;

            if (Path.IsPathRooted(exePath))
                return exePath;

            return Path.Combine(AppDomain.CurrentDomain.BaseDirectory, exePath);
        }

        /// <summary>True when a sunshine.exe process is currently running.</summary>
        public static bool IsRunning()
        {
            return RunningPath() != null;
        }

        /// <summary>
        /// The executable path of a running sunshine.exe process, or null when none.
        /// </summary>
        /// <returns>Full path of the first matching process, or null.</returns>
        public static string RunningPath()
        {
            foreach (var p in Process.GetProcessesByName("sunshine"))
            {
                try
                {
                    var path = p.MainModule?.FileName;
                    p.Dispose();
                    if (!string.IsNullOrWhiteSpace(path))
                    {
                        return path;
                    }
                }
                catch (Exception)
                {
                    p.Dispose();
                }
            }
            return null;
        }

        /// <summary>
        /// Launch sunshine.exe if it is not already running.
        /// </summary>
        /// <param name="exePath">Full path to sunshine.exe.</param>
        /// <returns>True once a sunshine process is running (or already was).</returns>
        public static bool StartIfNeeded(string exePath)
        {
            if (IsRunning())
                return true;

            var resolved = ResolveExePath(exePath);
            if (string.IsNullOrWhiteSpace(resolved) || !File.Exists(resolved))
                return false;

            var psi = new ProcessStartInfo
            {
                FileName = resolved,
                WorkingDirectory = Path.GetDirectoryName(resolved),
                UseShellExecute = false,
            };

            return Process.Start(psi) != null;
        }

        /// <summary>
        /// Wait until the Sunshine HTTPS API accepts connections.
        /// </summary>
        /// <param name="timeout">How long to wait before giving up.</param>
        /// <returns>True when the API responded with any HTTP status.</returns>
        public async Task<bool> WaitReadyAsync(TimeSpan timeout)
        {
            var deadline = DateTime.UtcNow + timeout;
            using var cts = new CancellationTokenSource(timeout);

            while (DateTime.UtcNow < deadline)
            {
                try
                {
                    using var req = new HttpRequestMessage(HttpMethod.Get, new Uri(new Uri(BaseUrl), "/api/csrf-token"));
                    using var resp = await _client.SendAsync(req, cts.Token).ConfigureAwait(false);
                    // Any HTTP response means the server is up.
                    return resp.StatusCode != HttpStatusCode.BadGateway && resp.StatusCode != HttpStatusCode.ServiceUnavailable && resp.StatusCode != HttpStatusCode.GatewayTimeout;
                }
                catch (Exception)
                {
                    await Task.Delay(500, cts.Token).ConfigureAwait(false);
                }
            }

            return false;
        }

        /// <summary>
        /// Send a request to switch the capture target. Option 1: switch to a window by handle.
        /// </summary>
        /// <param name="hwnd">Window handle of the target, or 0 for desktop.</param>
        /// <param name="xinputDelivery">Runtime XInput-over-UDP mode: "off", "global" or "process".</param>
        /// <param name="xinputPort">Runtime UDP base port for the XInput snapshot stream.</param>
        /// <returns>The HTTP status code returned by Sunshine.</returns>
        public Task<HttpStatusCode> SwitchToWindowAsync(IntPtr hwnd, string xinputDelivery, int xinputPort)
        {
            var body = BuildCaptureWindowBody("hwnd", hwnd.ToInt64(), xinputDelivery, xinputPort);
            return PostCaptureWindowAsync(body);
        }

        /// <summary>
        /// Send a request to switch the capture target. Option 2: switch by process id.
        /// </summary>
        /// <param name="pid">Process id of the target window.</param>
        /// <param name="xinputDelivery">Runtime XInput-over-UDP mode: "off", "global" or "process".</param>
        /// <param name="xinputPort">Runtime UDP base port for the XInput snapshot stream.</param>
        /// <returns>The HTTP status code returned by Sunshine.</returns>
        public Task<HttpStatusCode> SwitchToPidAsync(uint pid, string xinputDelivery, int xinputPort)
        {
            var body = BuildCaptureWindowBody("pid", pid, xinputDelivery, xinputPort);
            return PostCaptureWindowAsync(body);
        }

        /// <summary>
        /// Compose the /api/capture-window JSON body with an optional XInput delivery mode.
        /// The xinput fields are runtime-only and ride along with the capture switch.
        /// </summary>
        /// <param name="targetKey">"hwnd" or "pid" identifying the capture target.</param>
        /// <param name="targetValue">Value of the capture target.</param>
        /// <param name="xinputDelivery">XInput mode string: "off", "global" or "process".</param>
        /// <param name="xinputPort">UDP base port for the XInput snapshot stream.</param>
        /// <returns>The JSON request body.</returns>
        private static string BuildCaptureWindowBody(string targetKey, long targetValue, string xinputDelivery, int xinputPort)
        {
            var sb = new StringBuilder();
            sb.Append("{\"").Append(targetKey).Append("\":").Append(targetValue);
            sb.Append(",\"xinput_delivery\":\"").Append(xinputDelivery).Append('"');
            sb.Append(",\"xinput_port\":").Append(xinputPort);
            sb.Append('}');
            return sb.ToString();
        }

        private async Task<HttpStatusCode> PostCaptureWindowAsync(string body)
        {
            using var req = new HttpRequestMessage(HttpMethod.Post, new Uri(new Uri(BaseUrl), "/api/capture-window"))
            {
                Content = new StringContent(body, Encoding.UTF8, "application/json"),
            };

            var credentials = Convert.ToBase64String(Encoding.UTF8.GetBytes(Username + ":" + Password));
            req.Headers.TryAddWithoutValidation("Authorization", "Basic " + credentials);
            // Do NOT send Origin/Referer headers: non-browser clients skip CSRF validation,
            // otherwise we would have to fetch and echo back a CSRF token first.

            using var resp = await _client.SendAsync(req).ConfigureAwait(false);
            return resp.StatusCode;
        }

        public void Dispose()
        {
            _client.Dispose();
        }
    }
}