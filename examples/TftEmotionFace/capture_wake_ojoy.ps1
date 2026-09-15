param(
  [string]$Port = 'COM39',
  [int]$Seconds = 420,
  [ValidateRange(1, 40)]
  [int]$RecordSeconds = 40,
  [switch]$RecordNow,
  [switch]$WaitForWake,
  [switch]$DumpOnly,
  [string]$StopFile = ''
)
$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$outputDir = Join-Path $repoRoot ('.build/ojoy-diagnostics/' + (Get-Date -Format 'yyyyMMdd-HHmmss'))
New-Item -ItemType Directory -Force -Path $outputDir | Out-Null
$serial = New-Object System.IO.Ports.SerialPort $Port,115200,None,8,one
$serial.Encoding = [Text.Encoding]::UTF8
$serial.DtrEnable = $true
$serial.RtsEnable = $false
$serial.WriteTimeout = 1000
$log = New-Object Text.StringBuilder
$pending = ''
$started = $false
$recordStartedAt = -1.0
$stopRequested = $false
$dumpRequested = $false
$dumpAttempts = 0
$lengths = @(0, 0)
$chunks = @(@{}, @{})
try {
  $serial.Open()
  $serial.DiscardInBuffer()
  $watch = [Diagnostics.Stopwatch]::StartNew()
  if ($RecordNow) { $serial.Write('r'); $started = $true; $recordStartedAt = 0.0 }
  if ($DumpOnly) { $serial.Write('d'); $started = $true; $dumpRequested = $true }
  Write-Host "[capture] monitoring $Port; output: $outputDir"
  while ($watch.Elapsed.TotalSeconds -lt $Seconds) {
    # Allow a failed spoken test to be exported without closing the serial port.
    if ($StopFile -and $started -and !$stopRequested -and !$dumpRequested -and
        (Test-Path -LiteralPath $StopFile)) {
      $serial.Write('s')
      $stopRequested = $true
    }
    if (!$WaitForWake -and $recordStartedAt -ge 0 -and !$stopRequested -and
        $watch.Elapsed.TotalSeconds - $recordStartedAt -ge $RecordSeconds) {
      $serial.Write('s')
      $stopRequested = $true
    }
    $data = $serial.ReadExisting()
    if (!$data) { Start-Sleep -Milliseconds 10; continue }
    [void]$log.Append($data)
    $pending += $data
    while (($newline = $pending.IndexOf("`n")) -ge 0) {
      $line = $pending.Substring(0, $newline).TrimEnd("`r")
      $pending = $pending.Substring($newline + 1)
      if ($line -match '\[pcm:([01]):(\d+)\] ([A-Za-z0-9+/=]+)$') {
        $stage = [int]$Matches[1]
        $offset = [int]$Matches[2]
        try {
          $bytes = [Convert]::FromBase64String($Matches[3])
          $expected = [Math]::Min(96, $lengths[$stage] - $offset)
          if ($expected -gt 0 -and $bytes.Length -eq $expected) {
            $chunks[$stage][$offset] = $bytes
          }
        } catch { }
        continue
      }
      Write-Host ('[{0}] {1}' -f (Get-Date -Format 'HH:mm:ss'), $line)
      if ($WaitForWake -and $started -and !$stopRequested -and
          $line -match '\[wake\] detected:') {
        $serial.Write('s')
        $stopRequested = $true
      }
      if (!$started -and $line -match '\[xiaozhi\] state=speaking|client=speaking') {
        $serial.Write('r')
        $started = $true
        $recordStartedAt = $watch.Elapsed.TotalSeconds
      }
      if ($line -match '\[wake-pcm\] (?:ready|begin) (\d+) (\d+)') {
        $lengths = @([int]$Matches[1], [int]$Matches[2])
        if (!$dumpRequested) {
          $serial.Write('d')
          $dumpRequested = $true
        }
      }
      if ($line -match '\[wake-pcm\] end') {
        $missing = 0
        for ($stage = 0; $stage -lt 2; $stage++) {
          for ($offset = 0; $offset -lt $lengths[$stage]; $offset += 96) {
            if (!$chunks[$stage].ContainsKey($offset)) { $missing++ }
          }
        }
        if ($missing -eq 0) { break }
        if (++$dumpAttempts -gt 2) { throw "$missing PCM chunks missing after retries" }
        Write-Host "[capture] retrying export for $missing missing chunks"
        $serial.Write('d')
      }
    }
    if ($line -match '\[wake-pcm\] end' -and $missing -eq 0) { break }
  }
} finally {
  if ($serial.IsOpen) {
    if ($started -and !$stopRequested -and !$dumpRequested) { $serial.Write('s') }
    $serial.Close()
  }
  $serial.Dispose()
  [IO.File]::WriteAllText((Join-Path $outputDir 'serial.log'), $log.ToString(), (New-Object Text.UTF8Encoding($false)))
}
for ($stage = 0; $stage -lt 2; $stage++) {
  if ($lengths[$stage] -eq 0) { throw 'No PCM capture was exported' }
  $pcm = New-Object byte[] $lengths[$stage]
  for ($offset = 0; $offset -lt $pcm.Length; $offset += 96) {
    if (!$chunks[$stage].ContainsKey($offset)) { throw "Missing PCM chunk $stage at $offset" }
    [Array]::Copy($chunks[$stage][$offset], 0, $pcm, $offset, $chunks[$stage][$offset].Length)
  }
  $name = @('mic-reference', 'afe-output')[$stage]
  [IO.File]::WriteAllBytes((Join-Path $outputDir ($name + '.pcm')), $pcm)
}
Write-Host "PASS complete local PCM capture: $outputDir"
