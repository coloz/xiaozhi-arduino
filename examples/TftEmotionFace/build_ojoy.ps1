param(
  [switch]$Upload,
  [switch]$WakeDiagnostics,
  [string]$Port = 'COM39',
  [string]$GrokEyesLibrary = 'D:\Git\grokEyes',
  [string]$TftLibrary = 'D:\Git\TFT_eSPI'
)
$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$outputPath = Join-Path $repoRoot '.build/tft-emotion-ojoy'
$setupHeader = (Join-Path $PSScriptRoot 'ojoy_tft_setup.h').Replace('\', '/')
$modelPath = Join-Path $PSScriptRoot 'models/srmodels.bin'
$modelHash = '7C87DD7ADB5A7623907B6354D49BEA7F9289371262E6A57F15458FB8908C5814'
if ((Get-FileHash -LiteralPath $modelPath -Algorithm SHA256).Hash -ne $modelHash) {
  throw 'Unexpected wake model image; expected wn9_nihaoxiaozhi_tts'
}
if (!(Test-Path -LiteralPath (Join-Path $TftLibrary 'TFT_Drivers/CH13613_Init.h'))) {
  throw 'Select a TFT_eSPI fork with CH13613 support using -TftLibrary'
}
$fqbn = 'esp32:esp32:esp32s3:FlashMode=qio,FlashSize=16M,PartitionScheme=esp_sr_16,PSRAM=opi,USBMode=hwcdc,CDCOnBoot=cdc'
$flags = '-DXIAOZHI_BOARD=OJ_ESP32S3_OJOY -D_WS_CONFIG_MAX_MESSAGE_SIZE=16384 -include "' + $setupHeader + '"'
if ($WakeDiagnostics) { $flags += ' -DXIAOZHI_WAKE_DIAGNOSTICS=1' }
& arduino-cli compile --fqbn $fqbn --library $repoRoot --library $GrokEyesLibrary `
  --library $TftLibrary --libraries (Join-Path $repoRoot 'extras/arduino-libraries') `
  --build-path $outputPath --build-property "compiler.cpp.extra_flags=$flags" `
  --build-property "compiler.c.extra_flags=$flags" $PSScriptRoot
if ($LASTEXITCODE -ne 0) { throw 'OJoy compilation failed' }

# Arduino's build hook supplies the default hi-ESP model. Replace it before upload.
Copy-Item -LiteralPath $modelPath -Destination (Join-Path $outputPath 'srmodels.bin') -Force
if ($Upload) {
  & arduino-cli upload --fqbn $fqbn --port $Port --input-dir $outputPath $PSScriptRoot
  if ($LASTEXITCODE -ne 0) { throw 'OJoy upload failed' }
}
