param(
  [ValidateSet('64','32')]
  [string]$Arch = '64'
)
# smoke_wgi.ps1
# ------------------------------------------------------------------
# Validates the Windows.Gaming.Input fake layer added for UE5 titles
# (XInputFakeWgi=1):
#
#   Phase A - XInputFakeWgi=1 ONLY, no XInputFakePad, no UDP. The
#   injected layer must serve the WinRT Windows.Gaming.Input.Gamepad
#   activation with one connected, neutral pad, exactly the path Split
#   Fiction reads instead of XInput: RoGetActivationFactory (worker log
#   'wgi: Gamepad activation intercepted'), get_Gamepads (WGILIST
#   size=1), the IIterable range-for walk (WGITER count=1),
#   GetCurrentReading (WGIREAD btn=0, neutral) and put_Vibration
#   (WGIVIB). add_GamepadAdded must accept the subscription exactly
#   once (WGIADD token=1) and NEVER invoke it (a WGIFIRE line fails the
#   run). The real statics on this machine (no controller) hands back an
#   empty list, so the markers can only come from the DLL.
#
#   Phase B - XInputRewrite=1 + XInputFakeWgi=1 + loopback UDP feed.
#   A 24-byte XInputUdpPacket (magic XIP1, slot 0, A held) is streamed
#   to 127.0.0.1:45690; the WGI reading must map the XInput button
#   through (WGIREAD btn=4 - WGI GamepadButtonsFlags::A), the XInput
#   fast probe must serve the same state (SERVED xbtn=4096) and the
#   heartbeat must report wgifresh>=1.
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
  # The worker sleeps 500 ms first and the install needs ~3 s; assert
  # AFTER it has definitely finished so the worker-log lines exist.
  Start-Sleep -Seconds 5
}

function Assert-Marker($proc, $marker, $label) {
  $la = Join-Path $dir "logs\focusspoof_target_$($proc.Id).log"
  $deadline = [DateTime]::UtcNow.AddSeconds(12)
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

function WorkerHas($pattern) {
  $deadline = [DateTime]::UtcNow.AddSeconds(12)
  while ([DateTime]::UtcNow -lt $deadline) {
    $wl2 = Get-Content (Join-Path $dir "logs\focusspoof_worker.log") -ErrorAction SilentlyContinue
    if ($wl2 | Where-Object { $_ -match $pattern }) { return $true }
    Start-Sleep -Milliseconds 400
  }
  return $false
}

function WorkerAssert($pattern, $label) {
  if (WorkerHas $pattern) { Write-Host "$label OK"; return $true }
  Write-Host "$label FAILED (missing '$pattern')"
  return $false
}

$pass = $true
$null = Remove-Item (Join-Path $dir "logs\*") -Force -ErrorAction SilentlyContinue

# ------------------- Phase A: WGI layer alone ------------------------
@"
SuspendThreadsOnPatch=1
DisableFocusSpoof=0
EnableSubclass=0
XInputFakeWgi=1
"@ | Set-Content -Path $configPath -Encoding ASCII

$pa = Start-Process -FilePath $targetExe -ArgumentList 'A' -PassThru
Start-Sleep -Seconds 2
Invoke-Inject $pa
if ($pa.HasExited) {
  Write-Host "A DIED after inject (exit $($pa.ExitCode)) -> PHASE A FAILED"
  $pass = $false
} else {
  Write-Host "A alive; checking WGI markers..."
  $pass = (Assert-Marker $pa 'WGILIST t=\d+ size=1' 'WGI LIST (one fake pad in the statics list)') -and $pass
  $pass = (Assert-Marker $pa 'WGITER t=\d+ count=1' 'WGI ITERATOR (range-for walk finds the pad)') -and $pass
  $pass = (Assert-Marker $pa 'WGIREAD t=\d+ btn=0 lt=0.000 rt=0.000' 'WGI READING (neutral GetCurrentReading)') -and $pass
  $pass = (Assert-Marker $pa 'WGIVIB t=\d+' 'WGI VIBRATION (put_Vibration accepted)') -and $pass
  $pass = (Assert-Marker $pa 'WGIADD t=\d+ token=1' 'WGI ADDED SUBSCRIPTION (accepted, slot 0)') -and $pass
  $fire = Get-Content (Join-Path $dir "logs\focusspoof_target_$($pa.Id).log") -ErrorAction SilentlyContinue |
      Where-Object { $_ -match 'WGIFIRE' } | Select-Object -First 1
  if ($fire) { Write-Host "WGI ADDED FIRED (must never happen: $fire)"; $pass = $false }
  else { Write-Host "WGI ADDED FIRED CHECK OK (no GamepadAdded event)" }
  $pass = (WorkerAssert 'wgi: fake gamepad armed \(XInputFakeWgi=1\)' "WORKER 'wgi: fake gamepad armed'") -and $pass
  $pass = (WorkerAssert 'wgi: RoGetActivationFactory hooked' "WORKER 'wgi: RoGetActivationFactory hooked'") -and $pass
  $pass = (WorkerAssert 'wgi: Gamepad activation intercepted' "WORKER 'wgi: Gamepad activation intercepted'") -and $pass
  $pass = (WorkerAssert 'wgi: get_Gamepads serving the fake pad' "WORKER 'wgi: get_Gamepads serving the fake pad'") -and $pass
  $pass = (WorkerAssert 'wgi: GetCurrentReading served' "WORKER 'wgi: GetCurrentReading served'") -and $pass
  $pass = (WorkerAssert 'wgi: put_Vibration consumed' "WORKER 'wgi: put_Vibration consumed'") -and $pass
  $pass = (WorkerAssert 'wgi: GamepadAdded subscribed' "WORKER 'wgi: GamepadAdded subscribed'") -and $pass
  if (-not $pa.HasExited) { $pa.CloseMainWindow() | Out-Null; $null = $pa.WaitForExit(5000); if (-not $pa.HasExited) { $pa.Kill() | Out-Null } }
}

# ------------ Phase B: WGI + rewrite fed over loopback UDP ------------
@"
SuspendThreadsOnPatch=1
DisableFocusSpoof=0
EnableSubclass=0
XInputRewrite=1
XInputUdpMode=1
XInputUdpPort=45690
XInputFakeWgi=1
"@ | Set-Content -Path $configPath -Encoding ASCII

$pb = Start-Process -FilePath $targetExe -ArgumentList 'B' -PassThru
Start-Sleep -Seconds 2
Invoke-Inject $pb
if ($pb.HasExited) {
  Write-Host "B DIED after inject (exit $($pb.ExitCode)) -> PHASE B FAILED"
  $pass = $false
} else {
  Write-Host "B alive; streaming XIP1 packets with A held..."
  # 24-byte XInputUdpPacket: magic XIP1, slot 0, packetNumber, then a
  # 12-byte XINPUT_GAMEPAD with wButtons=0x1000 (XINPUT_GAMEPAD_A).
  $udp = New-Object System.Net.Sockets.UdpClient
  $udp.Connect('127.0.0.1', 45690)
  $magic = [BitConverter]::GetBytes([UInt32]0x31504958)
  $index = [BitConverter]::GetBytes([UInt32]0)
  $btns  = [BitConverter]::GetBytes([UInt16]0x1000)
  $pn = 0
  $deadline = [DateTime]::UtcNow.AddSeconds(14)
  while ([DateTime]::UtcNow -lt $deadline -and -not $pb.HasExited) {
    $bytes = New-Object 'byte[]' 24
    $magic.CopyTo($bytes, 0)
    $index.CopyTo($bytes, 4)
    [BitConverter]::GetBytes([UInt32]$pn).CopyTo($bytes, 8)
    $btns.CopyTo($bytes, 12)
    $udp.Send($bytes, $bytes.Length) | Out-Null
    $pn++
    Start-Sleep -Milliseconds 40
  }
  $udp.Close()
  Write-Host "feeder stopped after $pn packets; checking markers..."
  $pass = (Assert-Marker $pb 'WGIREAD t=\d+ btn=4 lt=0.000' 'WGI FED READING (XInput A maps to WGI A=0x4)') -and $pass
  $pass = (Assert-Marker $pb 'SERVED t=\d+ xbtn=4096' 'XINPUT FED STATE (same slot serves XInput A=0x1000)') -and $pass
  $pass = (WorkerAssert 'heartbeat .*wgifresh=[1-9]' "WORKER 'heartbeat wgifresh>=1 (fresh UDP state flows into WGI)'") -and $pass
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

if ($pass) { Write-Host "WGI SMOKE TEST PASSED ($Arch-bit)" }
else { Write-Host "WGI SMOKE TEST FAILED ($Arch-bit)" }
