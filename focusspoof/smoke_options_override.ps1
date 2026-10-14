param(
  [ValidateSet('64','32')]
  [string]$Arch = '64'
)
# smoke_options_override.ps1
# ------------------------------------------------------------------
# Validates the per-game "OptionsFile=" pointer added for GameArchives:
#
#   The DLL reads focus_options.txt next to itself at DLL attach. If that
#   file contains "OptionsFile=<absolute path>", every later config read is
#   redirected to the per-game file instead, so concurrently launched games
#   can never overwrite each other's focus options.
#
# Scenario: the SHARED focus_options.txt explicitly sets BlockPollingApis=0
# and the PER-GAME file sets BlockPollingApis=1 (plus the TargetWindow). If
# the pointer redirect works the injected process must swallow key messages
# (key_blocked:1 in the target log). If the redirect is broken the DLL reads
# the shared file and blocking stays OFF, failing the test loudly.
# ------------------------------------------------------------------
$ErrorActionPreference = 'Continue'
Set-StrictMode -Off

$dir = Split-Path -Parent $PSCommandPath
$injectedll = if ($Arch -eq '32') { Join-Path $dir "injectedll32.exe" } else { Join-Path $dir "injectedll.exe" }
$dllPath    = if ($Arch -eq '32') { Join-Path $dir "focusspoof32.dll" } else { Join-Path $dir "focusspoof.dll" }
$targetExe  = if ($Arch -eq '32') { Join-Path $dir "testtarget32.exe" } else { Join-Path $dir "testtarget.exe" }

$configPath    = Join-Path $dir "focus_options.txt"
$perGamePath   = Join-Path $dir "focus_options_demo.txt"

# 1. Shared file is JUST the pointer (no blocking keys at all).
"OptionsFile=$perGamePath" | Set-Content -Path $configPath -Encoding ASCII

# 2. Per-game file: known-good config with BlockPollingApis=1
@"
SuspendThreadsOnPatch=1
DisableFocusSpoof=0
EnableSubclass=1
BlockPollingApis=1
BlockWmInput=1
BlockRawInputApis=1
BlockCursorHide=1
BlockCursorLock=1
"@ | Set-Content -Path $perGamePath -Encoding ASCII

Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class Win32Opt {
  public delegate bool EnumWindowsProc(IntPtr h, IntPtr lp);
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumWindowsProc cb, IntPtr lp);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
}
'@

function Get-ProcMainWindow($proc) {
  $script:found = [IntPtr]::Zero
  $script:targetPid = $proc.Id
  $cb = [Win32Opt+EnumWindowsProc]{
    param($h, $lp)
    $wpid = [uint32]0
    [void][Win32Opt]::GetWindowThreadProcessId($h, [ref]$wpid)
    if ($wpid -eq [uint32]$script:targetPid -and [Win32Opt]::IsWindowVisible($h)) {
      $script:found = $h
      return $false
    }
    return $true
  }
  [void][Win32Opt]::EnumWindows($cb, [IntPtr]::Zero)
  $script:found
}

$pa = Start-Process -FilePath $targetExe -ArgumentList 'A' -PassThru
Start-Sleep -Seconds 3
$hA = Get-ProcMainWindow $pa
Write-Host "A pid=$($pa.Id) hwnd=$hA"

# TargetWindow goes into the PER-GAME file (the shared file never carries it).
Add-Content -Path $perGamePath -Value ("TargetWindow=" + $hA.ToInt64()) -Encoding ASCII

$proc = Start-Process -FilePath $injectedll -ArgumentList @($pa.Id.ToString(), "`"$dllPath`"") `
    -NoNewWindow -Wait -PassThru `
    -RedirectStandardOutput (Join-Path $dir "inj_out.txt") `
    -RedirectStandardError (Join-Path $dir "inj_err.txt")
Write-Host ("injectedll exit: " + $proc.ExitCode)
Start-Sleep -Seconds 3

if ($pa.HasExited) {
  Write-Host "A DIED after inject (exit " + $pa.ExitCode + ") -> OPTIONS OVERRIDE TEST FAILED"
} else {
  Write-Host "A ALIVE after inject"
}

$la = "logs\focusspoof_target_$($pa.Id).log"
$workerLog = Join-Path $dir "logs\focusspoof_worker.log"

$pass = $true

# The per-game file's TargetWindow must reach the worker (redirect used).
if (Test-Path $workerLog) {
  $cl = Get-Content $workerLog -ErrorAction SilentlyContinue
  if ($cl | Where-Object { $_ -match "target window=.*from controller" }) {
    Write-Host "REDIRECT OK (target window taken from the PER-GAME file)"
  } else {
    Write-Host "REDIRECT FAILED (no 'target window=.. from controller'; worker did not read the per-game file)"
    $pass = $false
  }
} else {
  Write-Host "WORKER LOG MISSING: $workerLog"
  $pass = $false
}

# The shared file says BlockPollingApis=0, the per-game file says 1: the
# swallowed WM_KEYDOWN (key_blocked:1) only shows up if the pointer worked.
Start-Sleep -Seconds 3
$bl = Get-Content $la -ErrorAction SilentlyContinue | Where-Object { $_ -match "key_blocked:1" } | Select-Object -First 1
if ($bl) {
  Write-Host "KEY BLOCK OK (key_blocked:1 observed - DLL read BlockPollingApis=1 from the per-game file)"
} else {
  Write-Host "KEY BLOCK FAILED (no key_blocked:1 - DLL did NOT honor the OptionsFile pointer)"
  $pass = $false
}

Get-Content $la -ErrorAction SilentlyContinue | Select-Object -Last 5 | ForEach-Object { Write-Host "  $_" }

# Cleanup
if (-not $pa.HasExited) { $pa.CloseMainWindow() | Out-Null }
$exited = $pa.WaitForExit(8000)
if (-not $exited) { $pa.Kill() | Out-Null }

# Restore a pointer-free shared config at the STANDARD base (same block as the
# other smoke scripts) so the controller workflow is unaffected.
@"
SuspendThreadsOnPatch=1
DisableFocusSpoof=0
EnableSubclass=1
BlockPollingApis=1
BlockWmInput=1
BlockRawInputApis=1
BlockCursorHide=1
BlockCursorLock=1
"@ | Set-Content -Path $configPath -Encoding ASCII
Remove-Item -Path $perGamePath -ErrorAction SilentlyContinue

if ($pass) { Write-Host "OPTIONS OVERRIDE TEST PASSED" }
else { Write-Host "OPTIONS OVERRIDE TEST FAILED" }