# ManualAudioConfig

这个示例演示在一个 `.ino` 中直接填写音频配置，通过麦克风、扬声器和 Xiaozhi 官方服务完成语音对话。默认接线适用于 **NULLLAB AI-VOX3（ES8311）**，不需要显示屏。

打开 [ManualAudioConfig.ino](ManualAudioConfig.ino)，在 `makeAudioConfig()` 中修改引脚、采样率和增益即可。示例通过 `Es8311Audio.h` 选择音频实现，无需定义 `XIAOZHI_BOARD`、`XIAOZHI_AUDIO_PROFILE` 或 `BOARD_AUDIO_*` 宏。

## 硬件与依赖

默认使用 AI-VOX3 板载 ES8311、麦克风、扬声器接口及 BOOT 键。其他 ES8311 开发板也可使用，但需要按实际接线修改配置。

安装以下库：

- Xiaozhi Arduino：本仓库的库目录。
- ArduinoJson 7.x。
- ArduinoWebsockets 0.5.4 或更新版本。
- EspressifOpus 和 EspressifEs8311：可使用仓库 [extras/arduino-libraries](../../extras/arduino-libraries) 中附带的版本。

`WiFi` 和 `Wire` 由 ESP32 Arduino Core 提供。默认无需安装显示库，也不需要 ESP-SR 模型分区。
使用 Arduino IDE 时，将附带的 `EspressifOpus` 和 `EspressifEs8311` 文件夹分别复制到 sketchbook 的 `libraries` 目录；下面的命令行示例则直接通过 `--libraries` 指定它们。

## Arduino IDE 设置

| 选项 | 设置 |
| --- | --- |
| 开发板 | ESP32S3 Dev Module |
| ESP32 Arduino Core | 3.x，已编译验证的版本为 3.3.11 |
| Flash Size | 16 MB |
| PSRAM | OPI PSRAM |
| Partition Scheme | Huge APP（3 MB 应用分区） |
| USB CDC On Boot | 使用原生 USB 串口时选择 Enabled |
| 串口监视器波特率 | 115200 |

默认示例关闭本地唤醒，使用 BOOT 键或串口控制会话。若启用唤醒，需要另行配置模型和分区，见文末。

## 首次使用

1. 打开 `ManualAudioConfig.ino`，填写 Wi-Fi：

   ```cpp
   constexpr char kWifiSsid[] = "你的 Wi-Fi 名称";
   constexpr char kWifiPassword[] = "你的 Wi-Fi 密码";
   ```

2. 确认 `makeAudioConfig()` 中的接线与开发板一致，编译并上传。
3. 打开 115200 波特率的串口监视器。连接 Wi-Fi 后，示例自动向官方配置服务获取连接信息，无需手动填写 WebSocket 地址和令牌。
4. 如果出现 `[activation] code=... message=...`，按提示在小智控制台完成设备绑定。等待激活期间，示例每 30 秒重新请求配置。
5. 初始化完成后，按 BOOT 或在串口发送小写 `t` 开始对话。空闲或监听期间执行会话切换；正在播放回答时，同一操作会打断播放。

串口中的 `[state]` 输出会话状态，`[event]` 输出事件文本，`[setup]` 和 `[error:...]` 提供配置或运行错误信息。配置请求或初始化失败后，示例会间隔 30 秒重试。

## 修改音频配置

AI-VOX3 默认参数如下，I2S 数据方向均从 ESP32 的视角命名：

| 参数 | 默认值 / 字段 |
| --- | --- |
| ES8311 I2C SDA / SCL | GPIO13 / GPIO12，`hardware.es8311.i2cSda/i2cScl` |
| ES8311 I2C 地址 | `0x18`，Wire 使用的 7 位地址 |
| I2S 控制器 | I2S0，输入、输出共享时钟 |
| MCLK / BCLK / WS | GPIO11 / GPIO10 / GPIO8 |
| 扬声器数据 DOUT | GPIO7，`hardware.output.data` |
| 麦克风数据 DIN | GPIO9，`hardware.input.data` |
| 输入 / 输出采样率 | 16000 Hz，端点的 `sampleRate` |
| I2S 格式 | 16 位双声道 slot，取左声道录音 |
| 麦克风增益 | 30 dB，`hardware.es8311.microphoneGainDb` |
| 输出音量 | -12 dB，`hardware.es8311.outputVolumeDb` |
| BOOT 键 | GPIO0，`kChatButtonPin`，低电平按下 |

配置必须从工厂函数开始：

```cpp
auto config = I2sOpusAudioPort::Config::forCompiledProfile();
```

然后修改 `config.hardware` 等字段并返回。不要直接用 `Config{}` 替换这一行：它为旧代码保留了平坦字段模式，可能使你填写的 `hardware` 字段不生效。

示例先设置输出端点，再执行 `input = output` 复制共享时钟和格式，最后将输入数据脚改为 GPIO9。修改共享 I2S 采样率或时钟时，要保持两端配置一致。

`I2sOpusAudioPort audioPort(makeAudioConfig())` 会复制配置，因此应在构造音频对象之前完成修改。随后 `setup()` 把音频对象挂接到 Client，再由 Runtime 负责录音、编码和播放；无需自行调用 `audioPort.loop()`。

如需改为独立 I2S、PDM 或自定义 Codec，可换用对应音频入口，并调整端点及依赖。一个 sketch 只选一种入口，在一个翻译单元中包含一次，不与板型宏预设混用。完整字段和硬件限制见 [音频配置指南](../../AUDIO_PROFILES.md#在-ino-中填写配置对象)。

## 命令行编译

从 `xiaozhi-arduino` 库根目录执行，ArduinoJson 和 ArduinoWebsockets 需已安装：

```powershell
arduino-cli compile --fqbn "esp32:esp32:esp32s3:PSRAM=opi,FlashSize=16M,PartitionScheme=huge_app" --library . --libraries extras/arduino-libraries --build-path .build/manual-audio-config examples/ManualAudioConfig
```

该示例已通过 ESP32 Arduino Core 3.3.11 编译及链接，固件约 1.48 MB；尚未进行实板录音、播放和网络会话验证。

## 可选：启用本地唤醒

1. 在包含音频入口前添加 `#define XIAOZHI_AUDIO_ENABLE_WAKE_ESP_SR 1` 和 `#include <ESP_SR.h>`。
2. 将 `makeAudioConfig()` 中的 `config.enableWakeDetection = false` 改为 `true`。
3. 选择包含模型分区的配置，例如 `ESP SR 16M`，并烧录与目标唤醒词匹配的模型。
4. 按模型需要设置 `wakeModelKeyword`、`defaultWakeWord` 等字段，详见 [音频配置指南](../../AUDIO_PROFILES.md)。

仅修改运行时的 `enableWakeDetection` 无法启用未编译的 WakeNet。启用唤醒后仍可使用 BOOT 或串口控制会话。
