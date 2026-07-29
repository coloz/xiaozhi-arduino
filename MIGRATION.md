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

## 行为差异

- Arduino 的 `setup()/loop()` 取代 `app_main()` 和 Application 主任务。
- 不再自动选择板型、引脚、显示或音频设备；由 sketch 显式注入。
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
