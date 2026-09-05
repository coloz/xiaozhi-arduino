# TftEmotionFace

该示例在 `TftRobotEyes` 表情界面上整合了 Xiaozhi 官方服务、注册引导、
编译期可选音频硬件、Opus 编解码、可选 WakeNet 唤醒和 BOOT 键会话控制。

## 硬件与编译设置

- ESP32-S3，16 MB Flash，8 MB OPI PSRAM
- 分区：`ESP SR 16M`（`PartitionScheme=esp_sr_16`）
- USB CDC On Boot：Enabled，便于查看串口诊断
- 开发板音频接线由 `.ino` 开头的 `XIAOZHI_BOARD` 直接选择，不需要
  `BoardConfig.h`；完整选择方法见项目根目录的
  `AUDIO_PROFILES.md`
- TFT_eSPI 不支持构造函数传入引脚；请在已安装库的 `User_Setup.h` 或构建
  参数中配置显示控制器和引脚，本示例不再携带额外的显示配置头文件

`models/srmodels.bin` 只包含 Espressif ESP-SR 2.4.6 的
`wn9_nihaoxiaozhi_tts` 模型，不含 Arduino 核心默认的 `wn9_hiesp`。
SHA-256：
`7C87DD7ADB5A7623907B6354D49BEA7F9289371262E6A57F15458FB8908C5814`

Arduino-ESP32 的构建钩子会自动把默认模型复制到构建目录。因此命令行构建后、
上传前，需要用本示例模型覆盖构建产物：

```powershell
arduino-cli compile --fqbn "esp32:esp32:esp32s3:FlashSize=16M,PartitionScheme=esp_sr_16,PSRAM=opi,USBMode=hwcdc,CDCOnBoot=cdc" --build-path .build examples/TftEmotionFace
Copy-Item examples/TftEmotionFace/models/srmodels.bin .build/srmodels.bin -Force
arduino-cli upload -p <COM-port> --fqbn "esp32:esp32:esp32s3:FlashSize=16M,PartitionScheme=esp_sr_16,PSRAM=opi,USBMode=hwcdc,CDCOnBoot=cdc" --input-dir .build examples/TftEmotionFace
```

根据本机 Arduino 库安装方式，编译命令可能还需要相应的 `--library` 参数。

## 使用方式

- 空闲状态说“你好小智”开始对话。
- 按一下 BOOT 键也可开始或结束对话。
- 串口发送 `t` 可执行与 BOOT 键相同的会话切换，波特率为 115200。
- 服务器表情事件会输出为 `EMOTION:<enum>,<name>`，同时交给
  `TftRobotEyes` 显示；名称严格使用 Xiaozhi 的 21 个标准字符串。
- 未注册时屏幕显示 `xiaozhi.me` 和激活码，并自动刷新注册状态。
- ES8311 Profile 找不到 Codec，或任一 Profile 的 I2S/PDM 引脚、控制器、
  采样率/位宽不合法时，屏幕显示 `AUDIO ERROR`，详细原因输出到串口。
