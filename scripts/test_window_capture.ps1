#requires -Version 5.1
<#
.SYNOPSIS
  Launches Sunshine in window-capture mode for a target window.

.DESCRIPTION
  Finds a top-level window by its title (exact or wildcard), resolves its
  window handle (HWND), and starts Sunshine with --captureWindow so that the
  window_capture recorder captures that specific window.

  Generated HWNDs are decimal, matching the --captureWindow CLI parameter that
  sunshine parses with std::stoull.

.PARAMETER WindowTitle
  Title of the target window. Supports PowerShell wildcards (*, ?).
  The first matching window (in top-level z-order) is used.

.PARAMETER ProcessName
  Optional process image name (e.g. "notepad") used as a second filter;
  requires the window to belong to that process.

.PARAMETER SunshineExe
  Path to the Sunshine executable. Defaults to the in-tree build output.

.PARAMETER TimeoutSeconds
  How long to keep waiting for a matching window to appear before failing.

.PARAMETER CaptureProcess
  If set, also pass --captureProcess <pid> so the recorder can match the
  window's process independently.

.EXAMPLE
  PS> .\scripts\test_window_capture.ps1 -WindowTitle "记事本"

.EXAMPLE
  PS> .\scripts\test_window_capture.ps1 -WindowTitle "*Notepad*" -ProcessName notepad -TimeoutSeconds 15
#>
[CmdletBinding()]
param(
  [Parameter(Mandatory = $true, Position = 0)]
  [string]$WindowTitle,

  [string]$ProcessName = $null,

  [string]$SunshineExe = $null,

  [int]$TimeoutSeconds = 5,

  [switch]$CaptureProcess
)

Set-StrictMode -Version 2

Add-Type -TypeDefinition @"
using System;
using System.Text;
using System.Text.RegularExpressions;
using System.Runtime.InteropServices;

public static class NativeWindows {
  public delegate bool EnumWindowsProc(IntPtr hWnd, IntPtr lParam);

  [DllImport("user32.dll", CharSet = CharSet.Unicode)]
  public static extern bool EnumWindows(EnumWindowsProc lpEnumFunc, IntPtr lParam);

  [DllImport("user32.dll", CharSet = CharSet.Unicode)]
  public static extern int GetWindowText(IntPtr hWnd, StringBuilder lpString, int nMaxCount);

  [DllImport("user32.dll")]
  public static extern bool IsWindowVisible(IntPtr hWnd);

  [DllImport("user32.dll")]
  public static extern uint GetWindowThreadProcessId(IntPtr hWnd, out uint processId);

  public static long FindWindowByTitle(string pattern, string processName) {
    long result = 0;
    EnumWindows((hWnd, lParam) => {
      if (!IsWindowVisible(hWnd)) {
        return true;
      }
      var sb = new StringBuilder(512);
      GetWindowText(hWnd, sb, sb.Capacity);
      string title = sb.ToString();
      if (string.IsNullOrEmpty(title)) {
        return true;
      }
      if (!Regex.IsMatch(title, PatternToRegex(pattern), RegexOptions.IgnoreCase | RegexOptions.Singleline)) {
        return true;
      }
      if (!string.IsNullOrEmpty(processName)) {
        uint pid = 0;
        GetWindowThreadProcessId(hWnd, out pid);
        try {
          using (var p = System.Diagnostics.Process.GetProcessById((int)pid)) {
            if (!p.ProcessName.Equals(processName, StringComparison.OrdinalIgnoreCase)) {
              return true;
            }
          }
        }
        catch {
          return true;
        }
      }
      result = hWnd.ToInt64();
      return false;  // stop enumeration after first match
    }, IntPtr.Zero);
    return result;
  }

  private static string PatternToRegex(string pattern) {
    return "^" + Regex.Escape(pattern).Replace("\\*", ".*").Replace("\\?", ".") + "$";
  }
}
"@

function Resolve-SunshinePath {
  $candidate = $SunshineExe
  if (-not $candidate) {
    $candidate = Join-Path $PSScriptRoot "..\cmake-build-release\sunshine.exe"
  }
  $candidate = [System.IO.Path]::GetFullPath($candidate)
  if (-not (Test-Path -LiteralPath $candidate)) {
    throw "Sunshine executable not found: $candidate (use -SunshineExe)"
  }
  return $candidate
}

function Find-TargetWindow {
  $deadline = [DateTime]::Now.AddSeconds($TimeoutSeconds)
  while ([DateTime]::Now -lt $deadline) {
    $hwnd = [NativeWindows]::FindWindowByTitle($WindowTitle, $ProcessName)
    if ($hwnd -ne 0) {
      return $hwnd
    }
    Start-Sleep -Milliseconds 250
  }
  return 0
}

$sunshine = Resolve-SunshinePath

Write-Host "Looking for window matching '$WindowTitle'" -ForegroundColor Cyan
$hwnd = Find-TargetWindow
if ($hwnd -eq 0) {
  Write-Error "No visible top-level window matched '$WindowTitle' within ${TimeoutSeconds}s."
  exit 1
}

Write-Host "Found HWND: 0x$($hwnd.ToString('X')) ($hwnd)" -ForegroundColor Green

# The recorder DLL must sit next to the executable or in its recorder\ folder.
$dllDir = Join-Path (Split-Path $sunshine) "recorder"
$dll = Join-Path $dllDir "window_capture.dll"
if (-not (Test-Path -LiteralPath $dll)) {
  Write-Warning "window_capture.dll not found at $dll ; capture will fail at runtime."
}

$startArgs = @("--captureWindow", "$hwnd")
if ($CaptureProcess) {
  $proc = Get-Process | Where-Object { $_.MainWindowHandle -eq [IntPtr]$hwnd } | Select-Object -First 1
  if ($proc) {
    $startArgs += @("--captureProcess", "$($proc.Id)")
    Write-Host "Found owning process: $($proc.ProcessName) (PID $($proc.Id))" -ForegroundColor Cyan
  } else {
    Write-Warning "Could not resolve owning PID for HWND; skipping --captureProcess."
  }
}

Write-Host "Starting: $sunshine $($startArgs -join ' ')" -ForegroundColor Cyan
# Sunshine resolves SUNSHINE_ASSETS_DIR ("assets") relative to its working directory,
# so we must launch it from its own directory rather than inheriting the caller's CWD.
$exeDir = Split-Path $sunshine
$process = Start-Process -FilePath $sunshine -ArgumentList $startArgs -WorkingDirectory $exeDir -PassThru
Write-Host "Started Sunshine (PID $($process.Id))." -ForegroundColor Green

# Wait briefly and report if it crashed immediately.
Start-Sleep -Milliseconds 1500
if ($process.HasExited) {
  Write-Error "Sunshine exited immediately with code $($process.ExitCode). Check the log output."
  exit 1
}

exit 0