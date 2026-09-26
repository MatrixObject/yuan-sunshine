using System;
using System.ComponentModel;
using System.Windows.Media.Imaging;

namespace SunshineWindowController.Models
{
    /// <summary>
    /// A selectable capture target shown in the main window list.
    /// Implements INotifyPropertyChanged so the UI updates when
    /// Thumbnail is assigned asynchronously.
    /// </summary>
    public sealed class CaptureTarget : INotifyPropertyChanged
    {
        private string _title;
        private IntPtr _hwnd;
        private uint _pid;
        private BitmapSource _thumbnail;
        private string _subtitle;
        private bool _isFocusSpoofOn;

        /// <summary>The window title, or "Desktop" for the desktop entry.</summary>
        public string Title
        {
            get => _title;
            set { _title = value; OnPropertyChanged(nameof(Title)); }
        }

        /// <summary>Window handle of the target, or 0 for the desktop.</summary>
        public IntPtr Hwnd
        {
            get => _hwnd;
            set { _hwnd = value; OnPropertyChanged(nameof(Hwnd)); OnPropertyChanged(nameof(IsDesktop)); OnPropertyChanged(nameof(PidText)); OnPropertyChanged(nameof(CanFocusSpoof)); }
        }

        /// <summary>Process ID owning the window, or 0 for the desktop.</summary>
        public uint Pid
        {
            get => _pid;
            set { _pid = value; OnPropertyChanged(nameof(Pid)); OnPropertyChanged(nameof(PidText)); }
        }

        /// <summary>Thumbnail screenshot of the target, or null when unavailable.</summary>
        public BitmapSource Thumbnail
        {
            get => _thumbnail;
            set { _thumbnail = value; OnPropertyChanged(nameof(Thumbnail)); }
        }

        /// <summary>Secondary line shown under the title (process id / window handle).</summary>
        public string Subtitle
        {
            get => _subtitle;
            set { _subtitle = value; OnPropertyChanged(nameof(Subtitle)); }
        }

        /// <summary>Process identifier text to display.</summary>
        public string PidText => Pid == 0 ? "Desktop" : Pid.ToString();

        /// <summary>True when this entry represents the desktop capture.</summary>
        public bool IsDesktop => Hwnd == IntPtr.Zero;

        /// <summary>
        /// True while the focus-spoof DLL is currently armed for this target.
        /// Driven by the per-window "开启/关闭窗口伪聚焦" button.
        /// </summary>
        public bool IsFocusSpoofOn
        {
            get => _isFocusSpoofOn;
            set
            {
                if (_isFocusSpoofOn == value)
                    return;
                _isFocusSpoofOn = value;
                OnPropertyChanged(nameof(IsFocusSpoofOn));
                OnPropertyChanged(nameof(FocusButtonText));
            }
        }

        /// <summary>Only real windows (not the desktop) can be focus-spoofed.</summary>
        public bool CanFocusSpoof => !IsDesktop;

        /// <summary>Label for the per-window focus-spoof button.</summary>
        public string FocusButtonText => IsFocusSpoofOn ? "关闭窗口伪聚焦" : "开启窗口伪聚焦";

        /// <summary>Raised when any bound property changes.</summary>
        public event PropertyChangedEventHandler PropertyChanged;

        private void OnPropertyChanged(string name)
        {
            PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(name));
        }
    }
}
