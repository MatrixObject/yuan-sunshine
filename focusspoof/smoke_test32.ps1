$ErrorActionPreference = 'Continue'
Set-StrictMode -Off

$dir = Split-Path -Parent $PSCommandPath
$injectedll = Join-Path $dir "injectedll32.exe"
$dllPath    = Join-Path $dir "focusspoof32.dll"
$targetExe  = Join-Path $dir "testtarget32.exe"

# Isolate the DLL's settings file: point an isolated APPDATA at a temp dir
# with a known-good config (block ON so the smoke test can observe
# key_blocked:1). This never touches the real %APPDATA%.
$env:APPDATA = Join-Path $env:TEMP "focusspoof_smoke"
$smokeConfigDir = Join-Path $env:APPDATA "SunshineWindowController"
New-Item -ItemType Directory -Force -Path $smokeConfigDir | Out-Null
@"
BlockKeyboardMouse=1
SuspendThreadsOnPatch=1
DisableFocusSpoof=0
EnableSubclass=1
DisableRawInput=0
BlockLegacyMessages=1
BlockCursorHide=1
BlockCursorLock=1
"@ | Set-Content -Path (Join-Path $smokeConfigDir "focus_options.txt") -Encoding ASCII

Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class Win32 {
  public delegate bool EnumWindowsProc(IntPtr h, IntPtr lp);
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumWindowsProc cb, IntPtr lp);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
  [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int c);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
}
'@

function Get-ProcMainWindow($proc) {
  $script:found = [IntPtr]::Zero
  $script:targetPid = $proc.Id
  $cb = [Win32+EnumWindowsProc]{
    param($h, $lp)
    $wpid = [uint32]0
    [void][Win32]::GetWindowThreadProcessId($h, [ref]$wpid)
    if ($wpid -eq [uint32]$script:targetPid -and [Win32]::IsWindowVisible($h)) {
      $script:found = $h
      return $false
    }
    return $true
  }
  [void][Win32]::EnumWindows($cb, [IntPtr]::Zero)
  $script:found
}

# Launch two instances: A (will be injected) and B (steals the foreground)
$pa = Start-Process -FilePath $targetExe -ArgumentList 'A' -PassThru
$pb = Start-Process -FilePath $targetExe -ArgumentList 'B' -PassThru
Start-Sleep -Seconds 3

$hA = Get-ProcMainWindow $pa
$hB = Get-ProcMainWindow $pb
Write-Host "A pid=$($pa.Id) hwnd=$hA ; B pid=$($pb.Id) hwnd=$hB"

# Mirror the controller: hand the DLL A's window handle before injecting.
Add-Content -Path (Join-Path $smokeConfigDir "focus_options.txt") -Value ("TargetWindow=" + $hA.ToInt64()) -Encoding ASCII

# Force B to the real foreground
[void][Win32]::ShowWindow($hB, 5)
[void][Win32]::SetForegroundWindow($hB)
Start-Sleep -Milliseconds 800
$fg = [Win32]::GetForegroundWindow()
Write-Host "Foreground is now: $fg (expect B=$hB)"

$la = "focusspoof_target_$($pa.Id).log"
if (Test-Path $la) {
  Write-Host "A log before injection (expect ==me: 0, because B is front):"
  Get-Content $la | Select-Object -Last 2 | ForEach-Object { Write-Host "  $_" }
}

# Inject into A
$proc = Start-Process -FilePath $injectedll -ArgumentList @($pa.Id.ToString(), "`"$dllPath`"") `
    -NoNewWindow -Wait -PassThru `
    -RedirectStandardOutput (Join-Path $dir "inj_out.txt") `
    -RedirectStandardError (Join-Path $dir "inj_err.txt")
Write-Host ("injectedll exit: " + $proc.ExitCode)
Write-Host ("out: " + ((Get-Content (Join-Path $dir "inj_out.txt") -ErrorAction SilentlyContinue) -join ' '))
$err = (Get-Content (Join-Path $dir "inj_err.txt") -ErrorAction SilentlyContinue)
if ($err) { Write-Host ("err: " + ($err -join ' ')) }

# Worker thread sleeps 500 ms then installs hooks; give it time
Start-Sleep -Seconds 3

if (-not $pa.HasExited) {
  $live = $false
  $blocked = 0
  foreach ($t in $pa.Threads) {
    $st = [string]$t.ThreadState
    if ($st -eq "Wait") { $blocked++ }
    else { $live = $true }
  }
  Write-Host "A ALIVE after inject. threads not waiting=$live, waiting=$blocked (message-pump thread must be non-waiting or WaitingOnMessage)"
} else {
  Write-Host ("A DIED after inject, exit=" + $pa.ExitCode)
}

# A's log should now say GetForegroundWindow==me: 1 even though B really has the foreground
Start-Sleep -Seconds 3
Write-Host "A log after injection (expect ==me: 1 even though B is the real foreground):"
Get-Content $la -ErrorAction SilentlyContinue | Select-Object -Last 5 | ForEach-Object { Write-Host "  $_" }

# Focus spoof must be live (window B holds the real foreground).
$meOk = Get-Content $la -ErrorAction SilentlyContinue | Where-Object { $_ -match "me:1" } | Select-Object -First 1
if ($meOk) { Write-Host "FOCUS SPOOF OK (me:1 observed while B is the real foreground)" }
else { Write-Host "FOCUS SPOOF FAILED (no me:1 after injection)" }

# Input blocking asserts two layers now:
#   1. The window-procedure layer is live: the subclassed SwallowProc eats a
#      posted WM_KEYDOWN before it reaches testtarget's own WndProc, so a
#      key_blocked=1 line must appear in A's log.
#   2. The worker reached "focus spoof active" and installed the blocking
#      detours without crashing the process.
$workerLog = Join-Path $env:TEMP "focusspoof_worker.log"
if (Test-Path $workerLog) {
  $workerLines = Get-Content $workerLog
  if ($workerLines | Where-Object { $_ -match "focus spoof active" }) {
    Write-Host "WORKER OK ('focus spoof active' logged)"
  } else {
    Write-Host "WORKER INCOMPLETE (missing 'focus spoof active' in $workerLog)"
    $workerLines | Select-Object -Last 8 | ForEach-Object { Write-Host "  $_" }
  }
  if ($workerLines | Where-Object { $_ -match "target window=.*from controller" }) {
    Write-Host "CONTROLLER HANDLE OK (target window from TargetWindow=config)"
  } else {
    Write-Host "CONTROLLER HANDLE MISSING (DLL self-discovered target)"
  }
  $det = $workerLines | Where-Object { $_ -match "detour:" } | Select-Object -First 1
  if ($det) { Write-Host "DETOUR OK ($det)" }
  else { Write-Host "DETOUR MISSING (no detour install lines)" }
  if ($workerLines | Where-Object { $_ -match "block: window-procedure subclass present" }) {
    Write-Host "WNDPROC LAYER OK (window-procedure subclass on)"
  } else {
    Write-Host "WNDPROC LAYER OFF (no subclass; raw-API layer must cover input)"
  }
  if ($workerLines | Where-Object { $_ -match "block: GetRawInputData hooked" }) {
    Write-Host "RAW HOOKS OK (GetRawInputData/GetRawInputBuffer hooked)"
  } else {
    Write-Host "RAW HOOKS MISSING (raw input hooks not installed)"
  }
} else {
  Write-Host "WORKER LOG MISSING: $workerLog"
}

# The posted key must now actually be swallowed at the queue level.
$bl = Get-Content $la -ErrorAction SilentlyContinue | Where-Object { $_ -match "key_blocked:1" } | Select-Object -First 1
if ($bl) { Write-Host "KEY BLOCK OK (key_blocked:1 observed in A log)" }
else { Write-Host "KEY BLOCK FAILED (queue swallow did not absorb WM_KEYDOWN)" }

# Unload / re-enable via the DLL's exported control functions. The
# controller calls these through "injectedll32.exe --call" so a window can be
# unhooked and later re-enabled without re-injecting.
if (-not $pa.HasExited) {
  $stop = Start-Process -FilePath $injectedll `
      -ArgumentList @("--call", $pa.Id.ToString(), "`"$dllPath`"", "SpoofStop") `
      -NoNewWindow -Wait -PassThru `
      -RedirectStandardOutput (Join-Path $dir "call_out.txt") `
      -RedirectStandardError (Join-Path $dir "call_err.txt")
  Write-Host ("SpoofStop exit: " + $stop.ExitCode + " " + (((Get-Content (Join-Path $dir "call_out.txt") -ErrorAction SilentlyContinue) + (Get-Content (Join-Path $dir "call_err.txt") -ErrorAction SilentlyContinue)) -join ' '))
  Start-Sleep -Seconds 3
  if ($pa.HasExited) {
    Write-Host "UNLOAD FAILED (process died during SpoofStop)"
  } else {
    $afterStop = Get-Content $la -ErrorAction SilentlyContinue | Select-Object -Last 1
    Write-Host "A log after SpoofStop: $afterStop"
    Write-Host "UNLOAD OK (process alive after SpoofStop)"
  }

  $start = Start-Process -FilePath $injectedll `
      -ArgumentList @("--call", $pa.Id.ToString(), "`"$dllPath`"", "SpoofStart") `
      -NoNewWindow -Wait -PassThru `
      -RedirectStandardOutput (Join-Path $dir "call_out.txt") `
      -RedirectStandardError (Join-Path $dir "call_err.txt")
  Write-Host ("SpoofStart exit: " + $start.ExitCode + " " + (((Get-Content (Join-Path $dir "call_out.txt") -ErrorAction SilentlyContinue) + (Get-Content (Join-Path $dir "call_err.txt") -ErrorAction SilentlyContinue)) -join ' '))
  Start-Sleep -Seconds 3
  if ($pa.HasExited) {
    Write-Host "RE-HOOK FAILED (process died during SpoofStart)"
  } else {
    $afterStart = Get-Content $la -ErrorAction SilentlyContinue | Select-Object -Last 1
    Write-Host "A log after SpoofStart: $afterStart"
    if ($afterStart -match "me:1") {
      Write-Host "RE-HOOK OK (focus spoof live again after SpoofStart)"
    } else {
      Write-Host "RE-HOOK INCOMPLETE (focus spoof not re-armed)"
    }
  }
}

# Cursor-control hooks must be installed (BlockCursorHide/BlockCursorLock=1).
if (Test-Path $workerLog) {
  $workerLines = Get-Content $workerLog
  if ($workerLines | Where-Object { $_ -match "cursor: ShowCursor hooked" }) {
    Write-Host "CURSOR HIDE HOOK OK (ShowCursor hooked)"
  } else {
    Write-Host "CURSOR HIDE HOOK MISSING (ShowCursor not hooked)"
  }
  if ($workerLines | Where-Object { $_ -match "cursor: ClipCursor hooked" }) {
    Write-Host "CURSOR LOCK HOOK OK (ClipCursor hooked)"
  } else {
    Write-Host "CURSOR LOCK HOOK MISSING (ClipCursor not hooked)"
  }
}

# Cleanup: close both, verify the detach path does not hang
if (-not $pa.HasExited) { $pa.CloseMainWindow() | Out-Null }
$exitedA = $pa.WaitForExit(8000)
$pb.CloseMainWindow() | Out-Null
$pb.WaitForExit(8000) | Out-Null
if ($exitedA) { Write-Host "A exited cleanly (detach path OK)" }
else { Write-Host "A HUNG on exit (detach path stuck!)"; $pa.Kill() | Out-Null }
if ($pb.HasExited) { Write-Host "B exited cleanly" }
else { Write-Host "B HUNG on exit"; $pb.Kill() | Out-Null }

