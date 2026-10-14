param(
  [ValidateSet('64','32')]
  [string]$Arch = '64'
)
# smoke_fakepad.ps1
# ------------------------------------------------------------------
# Validates the XInput fake-pad presence layer added for no-gamepad
# streaming (XInputFakePad=1):
#
#   Phase A - XInputFakePad=1 ONLY, no UDP, no real pad anywhere. The
#   injected layer must answer EVERY presence probe as "one connected,
#   idle Xbox pad on slot 0": XInputGetCapabilities (FAKECAPS, subtype
#   must be XINPUT_DEVSUBTYPE_GAMEPAD=1), XInputGetBatteryInformation
#   (FAKEBAT, wired=1), XInputGetKeystroke (FAKEKEY, ERROR_EMPTY) and a
#   neutral XInputGetState / XInputGetStateEx (SERVED / SERVEDEX with
#   xbtn=0). A rumble write must also be acknowledged without reaching any
#   real device (FAKESET). On this machine with no controller attached the
#   real XInput
#   returns ERROR_DEVICE_NOT_CONNECTED for all of them, so the markers
#   can only come from the injected fake-pad layer.
#
#   The auto-rescan driver (XInputFakeRescan=1, with the fast-idle test
#   options) arms the PRESENCE layer only once the game window proves idle,
#   then posts the WM_DEVICECHANGE rescan sequence. The DMC5-style presence
#   probes must then see the fake through each gate: WMIFAKE (WMI
#   Win32_PNPEntity with an IG_00 device id) and HIDFAKE (SetupDi/HidD HID
#   device at VID 045E/PID 028E). The worker log must show the arm, the
#   WM_DEVICECHANGE post and the WMI adoption ("auto rescan: WMI XInput
#   filter ran") and the heartbeat must show armed=1.
#
#   Phase B - XInputRewrite=1 + XInputFakePad=1, no UDP. With zero
#   datagrams the rewrite has no fresh state; instead of falling through
#   to "disconnected" the fake pad must keep the game seeing a connected,
#   neutral pad (SERVED appears).
# ------------------------------------------------------------------
$ErrorActionPreference = 'Continue'
Set-StrictMode -Off

$dir = Split-Path -Parent $PSCommandPath
$injectedll = if ($Arch -eq '32') { Join-Path $dir "injectedll32.exe" } else { Join-Path $dir "injectedll.exe" }
$dllPath    = if ($Arch -eq '32') { Join-Path $dir "focusspoof32.dll" } else { Join-Path $dir "focusspoof.dll" }
$targetExe  = if ($Arch -eq '32') { Join-Path $dir "testtarget32.exe" } else { Join-Path $dir "testtarget.exe" }
$configPath = Join-Path $dir "focus_options.txt"

function Invoke-Inject($proc) {
  $p = Start-Process -FilePath $injectedll -ArgumentList @($proc.Id.ToString(), "`"$dllPath`"") `
      -NoNewWindow -Wait -PassThru `
      -RedirectStandardOutput (Join-Path $dir "inj_out.txt") `
      -RedirectStandardError (Join-Path $dir "inj_err.txt")
  Write-Host ("injectedll exit: " + $p.ExitCode)
  # The worker sleeps 500 ms first and the full install (XInput family +
  # DirectInput8 + raw-input) needs ~3 s; assert AFTER it has definitely
  # finished so the worker-log lines exist.
  Start-Sleep -Seconds 5
}

function Assert-Marker($proc, $marker, $label) {
  $la = Join-Path $dir "logs\focusspoof_target_$($proc.Id).log"
  # The auto-rescan driver arms the presence layer only after the idle
  # detection (~1-3 s) and the 2 s probe tick must come around afterwards, so
  # poll for up to ~10 s instead of asserting a single snapshot.
  $deadline = [DateTime]::UtcNow.AddSeconds(10)
  while ([DateTime]::UtcNow -lt $deadline -and -not $proc.HasExited) {
    $hit = Get-Content $la -ErrorAction SilentlyContinue | Where-Object { $_ -match $marker } | Select-Object -First 1
    if ($hit) { Write-Host "$label OK ($hit)" ; return $true }
    Start-Sleep -Milliseconds 400
  }
  $hit = Get-Content $la -ErrorAction SilentlyContinue | Where-Object { $_ -match $marker } | Select-Object -First 1
  if ($hit) { Write-Host "$label OK ($hit)" ; return $true }
  Write-Host "$label FAILED (missing '$marker')"
  return $false
}

$pass = $true
$null = Remove-Item (Join-Path $dir "logs\*") -Force -ErrorAction SilentlyContinue

# ------------------------- Phase A: fake pad only -------------------------
@"
SuspendThreadsOnPatch=1
DisableFocusSpoof=0
EnableSubclass=0
XInputFakePad=1
XInputFakeRescan=1
XInputFakeIdleProbes=3
XInputFakeIdleMaxMs=8000
XInputFakeRetries=2
"@ | Set-Content -Path $configPath -Encoding ASCII

$pa = Start-Process -FilePath $targetExe -ArgumentList 'A' -PassThru
Start-Sleep -Seconds 2
Invoke-Inject $pa
if ($pa.HasExited) {
  Write-Host "A DIED after inject (exit $($pa.ExitCode)) -> PHASE A FAILED"
  $pass = $false
} else {
  Write-Host "A alive; checking fake-pad presence markers..."
  $pass = (Assert-Marker $pa 'FAKECAPS t=\d+ subtype=1' 'FAKE CAPABILITIES (gamepad subtype)') -and $pass
  $pass = (Assert-Marker $pa 'FAKECAPSEX t=\d+ vid=045e pid=028e' 'FAKE EXTENDED CAPABILITIES (Xbox 360 VID/PID)') -and $pass
  $pass = (Assert-Marker $pa 'FAKEBAT t=\d+ type=1' 'FAKE BATTERY (wired)') -and $pass
  $pass = (Assert-Marker $pa 'FAKEKEY t=\d+' 'FAKE KEYSTROKE (ERROR_EMPTY)') -and $pass
  $pass = (Assert-Marker $pa 'FAKESET t=\d+' 'FAKE SETSTATE (rumble acknowledged, not passed through)') -and $pass
  $pass = (Assert-Marker $pa 'FAKEDSOUND t=\d+' 'FAKE DSOUND (audio GUIDs, connected)') -and $pass
  $pass = (Assert-Marker $pa 'FAKEAUDIOIDS t=\d+' 'FAKE AUDIOIDS (audio IDs, connected)') -and $pass
  $pass = (Assert-Marker $pa 'SERVED t=\d+ xbtn=0' 'FAKE STATE (neutral GetState)') -and $pass
  $pass = (Assert-Marker $pa 'SERVEDEX t=\d+ xbtn=0 unknown=0' 'FAKE STATE (neutral GetStateEx, validation field zero)') -and $pass
  $pass = (Assert-Marker $pa 'RAWFAKE t=\d+' 'RAW FAKE HID DEVICE (045E:028E via raw-input tree)') -and $pass
  $pass = (Assert-Marker $pa 'WMIFAKE t=\d+' 'WMI FAKE WIN32_PNPEntity (IG_00 visible to the game filter)') -and $pass
  $pass = (Assert-Marker $pa 'HIDFAKE t=\d+' 'HID FAKE DEVICE (SetupDi/HidD sees VID 045E/PID 028E)') -and $pass
  function WorkerHas($pattern) {
    # Poll: the auto-rescan arm + adoption lines are appended by the driver
    # thread shortly after install and the worker log grows from several
    # threads (install, auto-rescan, heartbeat), so a single snapshot can miss
    # the tail end. The file is small; re-reading is trivial.
    $deadline = [DateTime]::UtcNow.AddSeconds(10)
    while ([DateTime]::UtcNow -lt $deadline) {
      $wl2 = Get-Content (Join-Path $dir "logs\focusspoof_worker.log") -ErrorAction SilentlyContinue
      if ($wl2 | Where-Object { $_ -match $pattern }) { return $true }
      Start-Sleep -Milliseconds 400
    }
    return $false
  }
  if (WorkerHas "xinput: fake pad armed") {
    Write-Host "WORKER 'xinput: fake pad armed' OK"
  } else {
    Write-Host "WORKER 'xinput: fake pad armed' FAILED"
    $pass = $false
  }
  if (WorkerHas "Enable called \(first\)") {
    Write-Host "WORKER 'XInputEnable caught (first)' OK"
  } else {
    Write-Host "WORKER 'XInputEnable caught (first)' FAILED"
    $pass = $false
  }
  if (WorkerHas "raw-input: fake X360 HID device armed") {
    Write-Host "WORKER 'raw-input: fake X360 HID device armed' OK"
  } else {
    Write-Host "WORKER 'raw-input: fake X360 HID device armed' FAILED"
    $pass = $false
  }
  if (WorkerHas "raw-input: GetRawInputDeviceList hooked") {
    Write-Host "WORKER 'raw-input: GetRawInputDeviceList hooked' OK"
  } else {
    Write-Host "WORKER 'raw-input: GetRawInputDeviceList hooked' FAILED"
    $pass = $false
  }
  if (WorkerHas "worker: auto-rescan driver started \(XInputFakeRescan=1\)") {
    Write-Host "WORKER 'auto-rescan driver started' OK"
  } else {
    Write-Host "WORKER 'auto-rescan driver started' FAILED"
    $pass = $false
  }
  if (WorkerHas "auto rescan: fake pad armed \(attempt=1/2") {
    Write-Host "WORKER 'auto rescan: fake pad armed (two-stage, retries=2)' OK"
  } else {
    Write-Host "WORKER 'auto rescan: fake pad armed (two-stage, retries=2)' FAILED"
    $pass = $false
  }
  if (WorkerHas "auto rescan: posting WM_DEVICECHANGE DBT_DEVNODES_CHANGED") {
    Write-Host "WORKER 'auto rescan: WM_DEVICECHANGE posted to game' OK"
  } else {
    Write-Host "WORKER 'auto rescan: WM_DEVICECHANGE posted to game' FAILED"
    $pass = $false
  }
  if (WorkerHas "auto rescan: WMI XInput filter ran") {
    Write-Host "WORKER 'auto rescan: WMI XInput filter ran (pad adopted)' OK"
  } else {
    Write-Host "WORKER 'auto rescan: WMI XInput filter ran (pad adopted)' FAILED"
    $pass = $false
  }
  if (WorkerHas "heartbeat .*armed=1") {
    Write-Host "WORKER 'heartbeat armed=1 (presence layer live)' OK"
  } else {
    Write-Host "WORKER 'heartbeat armed=1 (presence layer live)' FAILED"
    $pass = $false
  }
  if (-not $pa.HasExited) { $pa.CloseMainWindow() | Out-Null; $null = $pa.WaitForExit(5000); if (-not $pa.HasExited) { $pa.Kill() | Out-Null } }
}

# ----------------- Phase B: rewrite + fake pad, no UDP --------------------
@"
SuspendThreadsOnPatch=1
DisableFocusSpoof=0
EnableSubclass=0
XInputRewrite=1
XInputUdpMode=1
XInputUdpPort=45690
XInputFakePad=1
XInputFakeRescan=1
XInputFakeIdleProbes=3
XInputFakeIdleMaxMs=8000
XInputFakeRetries=2
"@ | Set-Content -Path $configPath -Encoding ASCII

$pb = Start-Process -FilePath $targetExe -ArgumentList 'B' -PassThru
Start-Sleep -Seconds 2
Invoke-Inject $pb
if ($pb.HasExited) {
  Write-Host "B DIED after inject (exit $($pb.ExitCode)) -> PHASE B FAILED"
  $pass = $false
} else {
  Write-Host "B alive; checking rewrite+fake with idle delivery..."
  # No datagram ever arrives: the fake pad must keep the slot "connected".
  $pass = (Assert-Marker $pb 'SERVED t=\d+ xbtn=0' 'REWRITE+FAKE (neutral pad while delivery idle)') -and $pass
  if (-not $pb.HasExited) { $pb.CloseMainWindow() | Out-Null; $null = $pb.WaitForExit(5000); if (-not $pb.HasExited) { $pb.Kill() | Out-Null } }
}

# Restore the shared config to the standard base (fake pad / rewrite off).
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

if ($pass) { Write-Host "FAKE PAD SMOKE TEST PASSED ($Arch-bit)" }
else { Write-Host "FAKE PAD SMOKE TEST FAILED ($Arch-bit)" }