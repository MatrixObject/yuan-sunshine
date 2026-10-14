# smoke_udp_rewrite.ps1
# ------------------------------------------------------------------
# Phase C - validates the XInput rewrite end-to-end over loopback UDP,
# WITHOUT any real gamepad or virtual-pad driver:
#
#   XInputRewrite=1 + XInputUdpMode=1 + XInputFakePad=0
#
# The harness pushes datagrams from THIS process to 127.0.0.1:45690 with
# the exact wire layout Sunshine produces (xinput_udp_packet, 24 bytes:
# magic "XIP1", index, packetNumber, XINPUT_GAMEPAD mirror). testtarget.exe
# probes XInputGetState / XInputGetStateEx every ~20 ms; with no fake pad
# and no real device the ONLY way any probe can return ERROR_SUCCESS with
# the pushed button is the injected rewrite serving a FRESH datagram.
#
# Expected markers in logs\focusspoof_target_<pid>.log:
#   SERVED   t=... xbtn=4096   (XINPUT_GAMEPAD_A = 0x1000)
#   SERVEDEX t=... xbtn=4096   (same through the extended entry)
# and later cadence lines showing xstate:0 xbtn:4096 while A is held, then
# xstate:0 xbtn:0 after the slot expires (no further datagrams = idle pad).
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
$workerLog  = Join-Path $dir "logs\focusspoof_worker.log"

function Assert-Marker($proc, $marker, $label) {
  $la = Join-Path $dir "logs\focusspoof_target_$($proc.Id).log"
  $hit = Get-Content $la -ErrorAction SilentlyContinue | Where-Object { $_ -match $marker } | Select-Object -First 1
  if ($hit) { Write-Host "$label OK ($hit)" ; return $true }
  Write-Host "$label FAILED (missing '$marker')"
  return $false
}

$null = Remove-Item (Join-Path $dir "logs\*") -Force -ErrorAction SilentlyContinue

@"
SuspendThreadsOnPatch=1
DisableFocusSpoof=0
EnableSubclass=0
XInputRewrite=1
XInputUdpMode=1
XInputUdpPort=45690
XInputFakePad=0
"@ | Set-Content -Path $configPath -Encoding ASCII

$pc = Start-Process -FilePath $targetExe -ArgumentList 'C' -PassThru
Start-Sleep -Seconds 2
$p = Start-Process -FilePath $injectedll -ArgumentList @($pc.Id.ToString(), "`"$dllPath`"") `
    -NoNewWindow -Wait -PassThru `
    -RedirectStandardOutput (Join-Path $dir "inj_out.txt") `
    -RedirectStandardError (Join-Path $dir "inj_err.txt")
Write-Host ("injectedll exit: " + $p.ExitCode)
  # The worker sleeps 500 ms first and the full install (XInput family +
  # DirectInput8 + raw-input) needs ~3 s; wait past it so the armed log exists.
  Start-Sleep -Seconds 5

$wl = Get-Content $workerLog -ErrorAction SilentlyContinue
if (-not ($wl | Where-Object { $_ -match 'xinput: rewrite armed' })) {
  # Re-read once after a short pause: the worker log is appended by several
  # threads and a single snapshot can miss the tail end of the install.
  Start-Sleep -Milliseconds 1000
  $wl = Get-Content $workerLog -ErrorAction SilentlyContinue
}
if (-not ($wl | Where-Object { $_ -match 'xinput: rewrite armed' })) {
  Write-Host "WORKER 'xinput: rewrite armed' FAILED - abort"
  if (-not $pc.HasExited) { $pc.CloseMainWindow() | Out-Null; $null = $pc.WaitForExit(5000); if (-not $pc.HasExited) { $pc.Kill() | Out-Null } }
  exit 1
}
Write-Host "WORKER 'xinput: rewrite armed' OK"

# With XInputFakePad=0 the DMC5 presence layers (WMI provider, SetupDi/HidD
# virtual device) must stay OFF: the worker must log the skip line and the
# target must never see WMIFAKE / HIDFAKE.
$wl = Get-Content $workerLog -ErrorAction SilentlyContinue
if ($wl | Where-Object { $_ -match 'hid: virtual HID device skipped \(XInputFakePad=0\)' }) {
  Write-Host "WORKER 'hid: virtual HID device skipped (XInputFakePad=0)' OK"
} else {
  Write-Host "WORKER 'hid: virtual HID device skipped (XInputFakePad=0)' FAILED - abort"
  if (-not $pc.HasExited) { $pc.CloseMainWindow() | Out-Null; $null = $pc.WaitForExit(5000); if (-not $pc.HasExited) { $pc.Kill() | Out-Null } }
  exit 1
}

# --- push A-button datagrams (0x1000) for ~3 s, then idle for ~1.5 s ---
$client = New-Object System.Net.Sockets.UdpClient
$ep = New-Object System.Net.IPEndPoint([System.Net.IPAddress]::Loopback, 45690)
function Send-PadPacket([uint16]$btn, [int16]$lx, [int16]$ly, [uint32]$pktNo) {
  $bytes = New-Object byte[] 24
  [Array]::Copy([BitConverter]::GetBytes([uint32]0x31504958), 0, $bytes, 0, 4)   # magic "XIP1"
  [Array]::Copy([BitConverter]::GetBytes([uint32]0), 0, $bytes, 4, 4)             # index=0
  [Array]::Copy([BitConverter]::GetBytes([uint32]$pktNo), 0, $bytes, 8, 4)        # packetNumber
  [Array]::Copy([BitConverter]::GetBytes([uint16]$btn), 0, $bytes, 12, 2)         # wButtons
  # bLeftTrigger=0, bRightTrigger=0
  $bytes[14] = 0; $bytes[15] = 0
  [Array]::Copy([BitConverter]::GetBytes([int16]$lx), 0, $bytes, 16, 2)           # sThumbLX
  [Array]::Copy([BitConverter]::GetBytes([int16]$ly), 0, $bytes, 18, 2)           # sThumbLY
  $bytes[20] = 0; $bytes[21] = 0                                                   # sThumbRX
  $bytes[22] = 0; $bytes[23] = 0                                                   # sThumbRY
  $null = $client.Send($bytes, 24, $ep)
}
$pktNo = 0
foreach ($i in 1..60)  { Send-PadPacket 0x1000 0 0 $pktNo; $pktNo++; Start-Sleep -Milliseconds 50 }
foreach ($i in 1..30)  { Send-PadPacket 0 0 0 $pktNo; $pktNo++; Start-Sleep -Milliseconds 50 }
$client.Close()
Start-Sleep -Seconds 2

$pass = $true
$pass = (Assert-Marker $pc 'SERVED t=\d+ xbtn=4096' 'SERVED (A button via XInputGetState)') -and $pass
$pass = (Assert-Marker $pc 'SERVEDEX t=\d+ xbtn=4096 unknown=0' 'SERVEDEX (A button via XInputGetStateEx, validation field zero)') -and $pass

$la = Join-Path $dir "logs\focusspoof_target_$($pc.Id).log"
$log = Get-Content $la -ErrorAction SilentlyContinue
if ($log | Where-Object { $_ -match 'xstate:0 xbtn:4096' }) {
  Write-Host "CADENCE xstate:0 xbtn:4096 OK (held A visible at 2 s cadence)"
} else {
  Write-Host "CADENCE xstate:0 xbtn:4096 FAILED (A never observed on a steady poll)"
  $pass = $false
}
if ($log | Where-Object { $_ -match 'xstate:(0|1167) xbtn:0' }) {
  Write-Host "CADENCE xstate:0/1167 xbtn:0 OK (button released after idle expiry, no phantom press)"
} else {
  Write-Host "CADENCE xstate:0/1167 xbtn:0 FAILED (button stuck pressed after idle expiry)"
  $pass = $false
}
$log2 = Get-Content $la -ErrorAction SilentlyContinue
if ($log2 | Where-Object { $_ -match 'WMIFAKE' }) {
  Write-Host "NEGATIVE WMIFAKE FAILED (fake WMI provider leaked with XInputFakePad=0)"
  $pass = $false
} else {
  Write-Host "NEGATIVE WMIFAKE OK (no WMI fake with XInputFakePad=0)"
}
if ($log2 | Where-Object { $_ -match 'HIDFAKE' }) {
  Write-Host "NEGATIVE HIDFAKE FAILED (fake SetupDi/HidD device leaked with XInputFakePad=0)"
  $pass = $false
} else {
  Write-Host "NEGATIVE HIDFAKE OK (no SetupDi fake with XInputFakePad=0)"
}

if (-not $pc.HasExited) {
  $pc.CloseMainWindow() | Out-Null
  $null = $pc.WaitForExit(5000)
  if (-not $pc.HasExited) { $pc.Kill() | Out-Null }
}

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

if ($pass) { Write-Host "UDP REWRITE SMOKE TEST PASSED ($Arch-bit)" }
else { Write-Host "UDP REWRITE SMOKE TEST FAILED ($Arch-bit)"; exit 1 }