param(
  [string]$Port = 'COM39',
  [ValidateRange(20, 300)]
  [int]$Seconds = 45,
  [switch]$ToggleChat,
  [switch]$WakeStart,
  [switch]$WakeInterrupt
)
$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$logDir = Join-Path $repoRoot '.build/ojoy-logs'
New-Item -ItemType Directory -Force -Path $logDir | Out-Null
$logPath = Join-Path $logDir ('serial-' + (Get-Date -Format 'yyyyMMdd-HHmmss') + '.log')
$serial = New-Object System.IO.Ports.SerialPort $Port,115200,None,8,one
$serial.Encoding = [Text.Encoding]::UTF8
$serial.DtrEnable = $true
$serial.RtsEnable = $false
$serial.WriteTimeout = 1000
$log = New-Object Text.StringBuilder
$sentToggle = $false
$speakingAt = -1.0
$interruptOffset = -1
try {
  $serial.Open()
  $watch = [Diagnostics.Stopwatch]::StartNew()
  while ($watch.Elapsed.TotalSeconds -lt $Seconds) {
    $data = $serial.ReadExisting()
    if ($data) {
      [void]$log.Append($data)
      Write-Host -NoNewline $data
    }
    if ($log.ToString() -match 'No bootable app partitions|OTA app partition slot 0 is not bootable') {
      throw 'Incomplete or invalid application image; re-upload the firmware'
    }
    if (($ToggleChat -or $WakeStart -or $WakeInterrupt) -and !$sentToggle -and
        $log.ToString() -match '\[xiaozhi\] ready;|\[heartbeat\][^\r\n]*client=idle') {
      if ($WakeStart -or $WakeInterrupt) { $serial.Write('w') } else { $serial.Write('t') }
      $sentToggle = $true
      Write-Host '[verify] requested dialogue'
    }
    if ($WakeInterrupt -and $sentToggle -and $interruptOffset -lt 0) {
      $states = [regex]::Matches($log.ToString(), '\[xiaozhi\] state=(\w+)')
      $speaking = $states.Count -gt 0 -and $states[$states.Count - 1].Groups[1].Value -eq 'speaking'
      if (!$speaking) { $speakingAt = -1.0 }
      if ($speakingAt -lt 0 -and $speaking) {
        $speakingAt = $watch.Elapsed.TotalSeconds
      }
      if ($speakingAt -ge 0 -and $watch.Elapsed.TotalSeconds - $speakingAt -ge 1) {
        $interruptOffset = $log.Length
        $serial.Write('w')
        Write-Host '[verify] injected wake word during speech'
      }
    }
    Start-Sleep -Milliseconds 100
  }
} finally {
  if ($serial.IsOpen) { $serial.Close() }
  $serial.Dispose()
  [IO.File]::WriteAllText($logPath, $log.ToString(), (New-Object Text.UTF8Encoding($false)))
}
$content = $log.ToString()
if ($content -match 'Guru Meditation|Backtrace:|assert failed|watchdog|Failed to allocate memory|\[wake\][^\r\n]*failed|\[display\][^\r\n]*failed|ES8311 not detected') {
  throw "Device reported a failure; inspect $logPath"
}
$heartbeats = [regex]::Matches($content, '\[heartbeat\] eyes=ready wifi=3 client=(\w+)[^\r\n]*frames=(\d+)')
if ($content -match 'activation code=|registration guide displayed' -and $heartbeats.Count -lt 3) {
  throw "Registration is pending; bind the displayed activation code at xiaozhi.me, then rerun. Log: $logPath"
}
if ($heartbeats.Count -lt 3) { throw "Expected at least three connected display heartbeats; inspect $logPath" }
$previousFrames = -1L
foreach ($sample in $heartbeats) {
  $frames = [long]$sample.Groups[2].Value
  if ($frames -le $previousFrames) { throw "Frame counter stopped or device restarted; inspect $logPath" }
  if ($sample.Groups[1].Value -notmatch '^(idle|connecting|listening|speaking)$') {
    throw "Client is not ready; inspect $logPath"
  }
  $previousFrames = $frames
}
$wakeSamples = [regex]::Matches($content, '\[wake-status\] on=1 capture=0 reads=(\d+) partial=\d+ frames=(\d+)')
if ($wakeSamples.Count -ge 2) {
  $first = $wakeSamples[0]
  $last = $wakeSamples[$wakeSamples.Count - 1]
  if ([long]$last.Groups[1].Value - [long]$first.Groups[1].Value -gt 100 -and
      [long]$last.Groups[2].Value -le [long]$first.Groups[2].Value) {
    throw "Microphone samples advance but wake processing is stalled; inspect $logPath"
  }
}
if ($ToggleChat -and (!$sentToggle -or $content -notmatch '\[xiaozhi\] state=listening')) {
  throw "Dialogue did not enter listening; inspect $logPath"
}
if ($WakeInterrupt) {
  if ($interruptOffset -lt 0) { throw "No speech available to interrupt; inspect $logPath" }
  $afterInterrupt = $content.Substring($interruptOffset)
  if ($afterInterrupt -notmatch '\[wake\] serial trigger from state=speaking accepted=yes' -or
      $afterInterrupt -notmatch '\[wake\] detected:' -or
      $afterInterrupt -notmatch '\[xiaozhi\] state=listening') {
    throw "Wake interruption did not succeed from Speaking; inspect $logPath"
  }
}
Write-Host "PASS Wi-Fi, running client and increasing display frames; log: $logPath"
if ($ToggleChat) { Write-Host 'PASS serial dialogue trigger reached listening' }
if ($WakeInterrupt) { Write-Host 'PASS injected wake word interrupted Speaking; acoustic recognition needs a spoken test' }
