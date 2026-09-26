using System;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.Drawing.Imaging;
using System.Runtime.InteropServices;
using System.Windows;
using System.Windows.Interop;
using System.Windows.Media.Imaging;
using Size = System.Drawing.Size;

namespace SunshineWindowController.Services
{
    /// <summary>
    /// Captures thumbnails of windows via PrintWindow with CopyFromScreen fallback,
    /// and of the desktop via CopyFromScreen.
    /// </summary>
    internal static class ThumbnailCapture
    {
        private const uint PW_RENDERFULLCONTENT = 0x00000002;

        /// <summary>Maximum thumbnail width in pixels.</summary>
        private const int ThumbWidth = 240;

        private const int SM_CXSCREEN = 0;
        private const int SM_CYSCREEN = 1;

        private const int DWMWA_EXTENDED_FRAME_BOUNDS = 9;

        /// <summary>
        /// Serializes captures through a single gate: GDI+ print operations are
        /// not thread-safe and concurrent captures throw
        /// "The object is currently in use elsewhere".
        /// </summary>
        private static readonly object CaptureGate = new object();

        [DllImport("user32.dll")]
        private static extern bool PrintWindow(IntPtr hwnd, IntPtr hdc, uint nFlags);

        [DllImport("user32.dll")]
        private static extern bool GetClientRect(IntPtr hwnd, out RECT rect);

        [DllImport("user32.dll")]
        private static extern bool GetWindowRect(IntPtr hwnd, out RECT rect);

        [DllImport("gdi32.dll")]
        private static extern bool DeleteObject(IntPtr hObject);

        [DllImport("user32.dll")]
        private static extern int GetSystemMetrics(int nIndex);

        [DllImport("dwmapi.dll")]
        private static extern int DwmGetWindowAttribute(IntPtr hwnd, int dwAttribute, out RECT pvAttribute, int cbAttribute);

        [StructLayout(LayoutKind.Sequential)]
        private struct RECT
        {
            public int Left;
            public int Top;
            public int Right;
            public int Bottom;
        }

        /// <summary>
        /// Capture a thumbnail of a top-level window.
        /// Tries PrintWindow first; if the result is all black (common in remote
        /// sessions or with GPU-accelerated windows), falls back to CopyFromScreen
        /// on the window's on-screen rectangle.
        /// </summary>
        /// <param name="hwnd">Window handle.</param>
        /// <returns>The captured thumbnail, or null when the window cannot be captured.</returns>
        public static BitmapSource CaptureWindow(IntPtr hwnd)
        {
            lock (CaptureGate)
            {
                // --- Attempt 1: PrintWindow into the client area ---
                if (GetClientRect(hwnd, out var clientRect))
                {
                    var cw = clientRect.Right - clientRect.Left;
                    var ch = clientRect.Bottom - clientRect.Top;
                    if (cw > 0 && ch > 0)
                    {
                        using (var bmp = new Bitmap(cw, ch, PixelFormat.Format32bppArgb))
                        {
                            using (var g = Graphics.FromImage(bmp))
                            {
                                var hdc = g.GetHdc();
                                try { PrintWindow(hwnd, hdc, PW_RENDERFULLCONTENT); }
                                finally { g.ReleaseHdc(hdc); }
                            }

                            if (!IsAllBlack(bmp))
                                return ScaleToThumbnail(bmp);
                        }
                    }
                }

                // --- Attempt 2: CopyFromScreen using the window's on-screen rect ---
                RECT wr = default;
                if (DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, out wr, Marshal.SizeOf(typeof(RECT))) != 0)
                {
                    GetWindowRect(hwnd, out wr);
                }

                var sw = wr.Right - wr.Left;
                var sh = wr.Bottom - wr.Top;
                if (sw > 0 && sh > 0)
                {
                    try
                    {
                        using (var bmp = new Bitmap(sw, sh, PixelFormat.Format32bppArgb))
                        {
                            using (var g = Graphics.FromImage(bmp))
                            {
                                g.CopyFromScreen(wr.Left, wr.Top, 0, 0, new Size(sw, sh));
                            }

                            if (!IsAllBlack(bmp))
                                return ScaleToThumbnail(bmp);
                        }
                    }
                    catch (Exception)
                    {
                        // Capture failed (window off-screen, minimized, etc.); fall through.
                    }
                }

                return null;
            }
        }

        /// <summary>
        /// Capture a thumbnail of the current desktop.
        /// </summary>
        /// <returns>The captured thumbnail.</returns>
        public static BitmapSource CaptureDesktop()
        {
            lock (CaptureGate)
            {
                var width = GetSystemMetrics(SM_CXSCREEN);
                var height = GetSystemMetrics(SM_CYSCREEN);
                if (width <= 0 || height <= 0)
                    return null;

                using (var bmp = new Bitmap(width, height, PixelFormat.Format32bppArgb))
                {
                    using (var g = Graphics.FromImage(bmp))
                    {
                        // Do not call GetHdc() here: CopyFromScreen is a managed
                        // Graphics call and mixing it with a raw HDC throws
                        // "The object is currently in use elsewhere".
                        g.CopyFromScreen(0, 0, 0, 0, new Size(width, height));
                    }

                    return ScaleToThumbnail(bmp);
                }
            }
        }

        /// <summary>
        /// Captures thumbnails of windows via PrintWindow with CopyFromScreen fallback,
        /// and of the desktop via CopyFromScreen.
        /// </summary>
        private static void Log(string msg)
        {
            try { System.IO.File.AppendAllText(System.IO.Path.Combine(System.IO.Path.GetTempPath(), "thumb_capture.log"),
                                               DateTime.Now.ToString("HH:mm:ss") + " " + msg + "\n"); } catch { }
        }

        /// <summary>
        /// Fast check whether a bitmap contains only black pixels
        /// (sampling every 16th pixel along the middle rows).
        /// </summary>
        private static bool IsAllBlack(Bitmap bmp)
        {
            var w = bmp.Width;
            var h = bmp.Height;
            var step = Math.Max(1, w / 16);

            var locked = bmp.LockBits(
                new Rectangle(0, 0, w, h),
                ImageLockMode.ReadOnly,
                PixelFormat.Format32bppArgb);
            try
            {
                var stride = locked.Stride;
                var ptr = locked.Scan0;
                var buf = new byte[stride * h];
                System.Runtime.InteropServices.Marshal.Copy(ptr, buf, 0, buf.Length);

                for (int y = h / 4; y < h - h / 4; y += Math.Max(1, h / 8))
                {
                    var rowOff = y * stride;
                    for (int x = 0; x < w; x += step)
                    {
                        var off = rowOff + x * 4;
                        byte b = buf[off];
                        byte g = buf[off + 1];
                        byte r = buf[off + 2];
                        if (r + g + b > 30)      // any non-negligible color
                            return false;
                    }
                }
                return true;
            }
            finally
            {
                bmp.UnlockBits(locked);
            }
        }

        private static BitmapSource ScaleToThumbnail(Bitmap bmp)
        {
            var height = (int)Math.Round(bmp.Height * ((double)ThumbWidth / bmp.Width));
            if (height <= 0)
                return null;

            if (bmp.Width > ThumbWidth)
            {
                using (var scaled = new Bitmap(ThumbWidth, height, PixelFormat.Format32bppArgb))
                {
                    using (var g = Graphics.FromImage(scaled))
                    {
                        g.InterpolationMode = InterpolationMode.HighQualityBicubic;
                        g.DrawImage(bmp, 0, 0, scaled.Width, scaled.Height);
                    }

                    return ToBitmapSource(scaled);
                }
            }

            return ToBitmapSource(bmp);
        }

        private static BitmapSource ToBitmapSource(Bitmap bmp)
        {
            var hBitmap = bmp.GetHbitmap();
            try
            {
                var source = Imaging.CreateBitmapSourceFromHBitmap(hBitmap, IntPtr.Zero, Int32Rect.Empty, BitmapSizeOptions.FromEmptyOptions());
                source.Freeze();
                return source;
            }
            finally
            {
                if (hBitmap != IntPtr.Zero)
                    DeleteObject(hBitmap);
            }
        }

        /// <summary>
        /// Create a placeholder thumbnail for a window that could not be captured
        /// (e.g. minimized or off-screen). Shows the title on a dark background so
        /// the list entry is never a bare black box.
        /// </summary>
        /// <param name="title">Window title to draw.</param>
        /// <returns>A frozen BitmapSource placeholder.</returns>
        public static BitmapSource CreatePlaceholder(string title)
        {
            lock (CaptureGate)
            {
                using (var bmp = new Bitmap(ThumbWidth, 90, PixelFormat.Format32bppArgb))
                {
                    using (var g = Graphics.FromImage(bmp))
                    {
                        g.Clear(Color.FromArgb(32, 32, 34));
                        using (var font = new Font("Segoe UI", 9f))
                        {
                            var text = string.IsNullOrWhiteSpace(title) ? "No preview" : title;
                            using (var brush = new SolidBrush(Color.FromArgb(200, 200, 200)))
                            {
                                var rectF = new RectangleF(4, 4, bmp.Width - 8, bmp.Height - 8);
                                g.DrawString(text, font, brush, rectF, StringFormat.GenericTypographic);
                            }
                        }
                    }

                    return ToBitmapSource(bmp);
                }
            }
        }
    }
}
