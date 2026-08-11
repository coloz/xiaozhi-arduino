# 从 xiaozhi-esp32 2.4.0 迁移

## 模块映射

| 原 ESP-IDF 模块 | Arduino 版本 |
|---|---|
| `Application` 单例 | 可实例化的 `xiaozhi::Client` |
| `DeviceStateMachine` | `xiaozhi::StateMachine`，转换原子化，FatalError 可达 |
| `WebsocketProtocol` | `Protocol` + `Transport` + `ArduinoWebSocketTransport` |
| `McpServer`/cJSON | `McpServer`/ArduinoJson，处理器由 sketch 注册 |
| `Ota` 中的响应解析 | `Provisioning` 和 `ArduinoProvisioningClient` |
| `SystemInfo`/Board UUID | `Esp32Identity` |
| `AudioService` | `EncodedAudioPort` 扩展接口 |
| `AudioCodec` | 扩展侧 `AudioIo` 实现 |
| Display/LVGL/Assets | 不进入核心；事件回调或独立显示库 |
| ESP-SR/WakeNet/AEC | 不进入核心；独立音频算法库 |
| 137 个板配置 | sketch/板级适配包，不在通用库中编译 |

## 配置迁移

WebSocket 服务端配置对应：

```cpp
config.websocket_url = old_websocket_url;
config.authorization = old_websocket_token;
config.protocol_version = old_websocket_version;
config.device_id = xiaozhi::Esp32Identity::deviceId();
config.client_id = xiaozhi::Esp32Identity::persistentClientId();
```

`persistentClientId()` 延续原工程 `board/uuid` 的 Preferences/NVS 命名，升级现有设备时可以继续使用原 UUID。

### 音频配置

`TftEsPiDisplay` 和 `TftEmotionFace` 现在使用编译期音频 Profile，只把选中的麦克风、扬声器和 Codec 后端编译进固件。可选项包括 ES8311、共享时钟 I2S、独立 I2S（如 INMP441/MSM261 + MAX98357A）、PDM + MAX98357A 以及自定义 Codec；选择方法和硬件约束见 [AUDIO_PROFILES.md](AUDIO_PROFILES.md)。实现位于 `src/xiaozhi/audio` 的按需包含头文件中，未定义 `XIAOZHI_AUDIO_BOARD` 时不会引入这些依赖。

已有 sketch 直接填写 `I2sOpusAudioPort::Config` 的平坦字段仍保持源码兼容。新代码应先使用 `I2sOpusAudioPort::Config::forCompiledProfile()` 取得所选 Profile 的安全默认值，再填写对应端点；完整示例则通过 `.ino` 中的 `XIAOZHI_AUDIO_BOARD` 选择库内音频预设，由 `BoardAudioConfig.h` 的 `xiaozhi_audio_board::makeConfig()` 将 `BOARD_AUDIO_*` 宏转换为该配置。这样运行时调优仍可保留，同时未选中的后端在预处理阶段就被裁掉。

## 行为差异

- Arduino 的 `setup()/loop()` 取代 `app_main()` 和 Application 主任务。
- 不再自动选择显示设备或 UI 引脚；由 sketch 显式选择编译期音频板/Profile，并通过 `BoardAudioConfig.h` 或自定义配置注入音频引脚。TFT_eSPI/U8g2 的屏幕引脚仍由应用配置。
- 不再自动接受不校验证书的 WSS；内置适配器必须配置可信 CA。
- 原工程的重启、OTA 等 `user_only` MCP 工具不再仅靠“列表隐藏”保护；迁移后必须提供本地授权回调。
- WebSocket 是内置参考传输。Provisioning 仍会解析 MQTT 配置，供独立 MQTT+UDP 传输扩展消费。
- 本库接收和发送的是 Opus 包，不隐式绑定某个 ESP-IDF `esp_audio_codec` 版本。

## 已修复的原实现风险

- FatalError 没有合法入口。
- 状态校验和写入之间存在并发竞态。
- WebSocket v2/v3 在截断帧上越界，并修改 `const` 接收数据。
- v3 payload 的 16 位长度可能静默截断。
- server hello 的 transport 字段可能空指针解引用。
- 版本解析使用无保护 `stoi()`。
- MCP 通过字符串拼接错误消息，可能产生无效 JSON。
