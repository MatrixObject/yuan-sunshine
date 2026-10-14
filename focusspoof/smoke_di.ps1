# smoke_di.ps1
# ------------------------------------------------------------------
# Validates the DirectInput8 fake gamepad layer added for no-gamepad
# streaming (XInputFakePad=1). A DirectInput title with no driver finds no
# controller at all, so the injected layer must append one virtual wired
# X360 pad to EnumDevices and hand out a working IDirectInputDevice8 for it.
#
#   Phase A - XInputFakePad=1 ONLY. testtarget.exe enumerates
#   DI8DEVCLASS_GAMECTRL, creates the pad it finds, binds a gamepad data
#   format and reads state, and once probes the device identity, object
#   table, a property and the buffered path. Expected markers:
#       DIFOUND  t=... devtype=0215   (instance: std | gamepad, as real X360)
#       DICREATE t=...
#       DICAPS   t=... hr=0 axes=6 buttons=16 povs=1 devtype=10215 (HID | ...)
#       DINFO    t=... hr=0 inst=b1ade001 prod=028e045e devtype=10215
#       DOBJINFO t=... hr=0 ofs=0 type=00000002 (X axis, absolute)
#       DPROP    t=... hr=0                    (DIPROP_BUFFERSIZE accepts)
#       DDATA    t=... hr=0 rows=0             (buffered path stays empty)
#       DISTATE  t=... hr=0 ... b0=0   (neutral pad: no datagram yet)
#       DIWILD   t=... hr=0 bind=0 lx=... ly=... b0=0   (DMC5-style wildcard
#                instance buttons resolve to zero on the neutral pad)
#   On this machine with no controller attached a plain EnumDevices finds
#   no gamepad, so DIFOUND can only come from the injected fake device.
#
#   Phase B - XInputRewrite=1 + XInputUdpMode=1 + XInputFakePad=1. With an A
#   button held on the wire the fake DirectInput device must report button 0
#   pressed (b0=128, the DirectInput "down" bit for XINPUT_GAMEPAD_A), in
#   both the named-format state (DISTATE) and the DMC5-style wildcard-format
#   state (DIWILD), where the button number comes from the object offset.
# ------------------------------------------------------------------
param(
  [ValidateSet('64','32')]
  [string]$Arch = '64'
)
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
  Start-Sleep -Seconds 3
}

function Assert-Marker($proc, $marker, $label) {
  $la = Join-Path $dir "logs\focusspoof_target_$($proc.Id).log"
  # DI presentation is armed-gated: the auto-rescan driver arms it only after
  # the idle detection (~1-3 s) and the 2 s probe tick must come around
  # afterwards, so poll for up to ~10 s instead of a single snapshot.
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

function Stop-Target($proc) {
  if (-not $proc.HasExited) {
    $proc.CloseMainWindow() | Out-Null
    $null = $proc.WaitForExit(5000)
    if (-not $proc.HasExited) { $proc.Kill() | Out-Null }
  }
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

$pa = Start-Process -FilePath $targetExe -ArgumentList 'DI-A' -PassThru
Start-Sleep -Seconds 2
Invoke-Inject $pa
if ($pa.HasExited) {
  Write-Host "A DIED after inject (exit $($pa.ExitCode)) -> PHASE A FAILED"
  $pass = $false
} else {
  Start-Sleep -Seconds 4
  Write-Host "A alive; checking DirectInput fake-pad markers..."
  $pass = (Assert-Marker $pa 'DIFOUND t=\d+ devtype=0215' 'DI FAKE PAD ENUMERATED (HID gamepad)') -and $pass
  $pass = (Assert-Marker $pa 'DICREATE t=\d+' 'DI FAKE PAD CREATED') -and $pass
  $pass = (Assert-Marker $pa 'DICAPS t=\d+ hr=0 axes=6 buttons=16 povs=1 devtype=10215' 'DI FAKE PAD CAPABILITIES (X360 gamepad)') -and $pass
  $pass = (Assert-Marker $pa 'DINFO t=\d+ hr=0 inst=b1ade001 prod=028e045e devtype=10215' 'DI FAKE PAD DEVICEINFO (X360 identity)') -and $pass
  $pass = (Assert-Marker $pa 'DOBJINFO t=\d+ hr=0 ofs=0 type=00000002' 'DI FAKE PAD OBJECTINFO (X axis)') -and $pass
  $pass = (Assert-Marker $pa 'DPROP t=\d+ hr=0' 'DI FAKE PAD SETPROPERTY (buffersize)') -and $pass
  $pass = (Assert-Marker $pa 'DDATA t=\d+ hr=0 rows=0' 'DI FAKE PAD BUFFERED READ (empty, immediate mode)') -and $pass
  $pass = (Assert-Marker $pa 'DISTATE t=\d+ hr=0 .*b0=0 b1=0' 'DI FAKE PAD STATE (neutral)') -and $pass
  $pass = (Assert-Marker $pa 'DIWILD t=\d+ hr=0 bind=0 .*b0=0' 'DI WILDCARD FORMAT STATE (neutral)') -and $pass
  $pass = (Assert-Marker $pa 'WMIFAKE t=\d+' 'WMI FAKE WIN32_PNPEntity (IG_00 visible to the game filter)') -and $pass
  $pass = (Assert-Marker $pa 'HIDFAKE t=\d+' 'HID FAKE DEVICE (SetupDi/HidD sees VID 045E/PID 028E)') -and $pass
  $wlPoll = Join-Path $dir "logs\focusspoof_worker.log"
  $deadline = [DateTime]::UtcNow.AddSeconds(10)
  $wmiAdopted = $false
  while ([DateTime]::UtcNow -lt $deadline) {
    $wl2 = Get-Content $wlPoll -ErrorAction SilentlyContinue
    if ($wl2 | Where-Object { $_ -match "auto rescan: WMI XInput filter ran" }) { $wmiAdopted = $true; break }
    Start-Sleep -Milliseconds 400
  }
  if ($wmiAdopted) { Write-Host "WORKER 'auto rescan: WMI XInput filter ran (pad adopted)' OK" }
  else { Write-Host "WORKER 'auto rescan: WMI XInput filter ran (pad adopted)' FAILED" ; $pass = $false }
  Stop-Target $pa
}

# ----------------- Phase B: rewrite + fake pad, A held --------------------
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

$pb = Start-Process -FilePath $targetExe -ArgumentList 'DI-B' -PassThru
Start-Sleep -Seconds 2
Invoke-Inject $pb
if ($pb.HasExited) {
  Write-Host "B DIED after inject (exit $($pb.ExitCode)) -> PHASE B FAILED"
  $pass = $false
} else {
  Write-Host "B alive; pushing A-button datagrams..."
  $client = New-Object System.Net.Sockets.UdpClient
  $ep = New-Object System.Net.IPEndPoint([System.Net.IPAddress]::Loopback, 45690)
  $bytes = New-Object byte[] 24
  [Array]::Copy([BitConverter]::GetBytes([uint32]0x31504958), 0, $bytes, 0, 4)  # magic "XIP1"
  [Array]::Copy([BitConverter]::GetBytes([uint32]0), 0, $bytes, 4, 4)            # index=0
  [Array]::Copy([BitConverter]::GetBytes([uint16]0x1000), 0, $bytes, 12, 2)      # wButtons = A
  foreach ($i in 1..120) { $null = $client.Send($bytes, 24, $ep); Start-Sleep -Milliseconds 50 }
  $client.Close()
  Start-Sleep -Seconds 2
  $pass = (Assert-Marker $pb 'DISTATE t=\d+ hr=0 .*b0=128' 'DI FAKE PAD STATE (A pressed -> button 0)') -and $pass
  $pass = (Assert-Marker $pb 'DIWILD t=\d+ hr=0 bind=0 .*b0=128' 'DI WILDCARD FORMAT STATE (A pressed -> button 0 by offset)') -and $pass
  Stop-Target $pb
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

if ($pass) { Write-Host "DIRECTINPUT SMOKE TEST PASSED ($Arch-bit)" }
else { Write-Host "DIRECTINPUT SMOKE TEST FAILED ($Arch-bit)" ; exit 1 }
