# Xiaozhi Arduino

[English](README_EN.md) | 简体中文

这是基于小智项目的Arduino实现。
它不是把原 ESP-IDF 工程整体复制进 Arduino，而是保留语音会话必需的协议与状态逻辑，将显示、板卡和算法实现改成可选适配层。

## 设计目标

- 保持 Xiaozhi WebSocket 协议 v1/v2/v3、Opus 帧格式、会话状态和 MCP JSON-RPC 兼容。
- 不依赖 LVGL、字体、表情、音效资源、摄像头、LED、具体开发板或 ESP-SR。
- 不使用全局单例；一个 sketch 可以创建、替换或测试多个独立对象。
- 所有网络和音频输入都有大小、类型和边界检查。
- TLS 默认安全：`wss://` 必须提供可信 CA；若没有证书，只能改用自定义 Transport。

## 核心和扩展边界

| 放在本库核心中 | 由 Arduino/板级扩展提供 |
|---|---|
| 会话状态机和事件 | LVGL/U8g2/屏幕 UI |
| WebSocket v1/v2/v3 编解码 | I2S 和具体音频 Codec 芯片 |
| hello/listen/abort/STT/TTS/LLM 消息 | PCM↔Opus 编解码器与重采样 |
| MCP 工具注册、参数校验和分页 | ESP-SR、WakeNet、VAD、AEC |
| OTA/激活配置解析与 HTTP 获取 | 摄像头、LED、按钮、电源管理 |
| ESP32 MAC、持久 UUID、固件版本 | 字体、表情、OGG 和 assets 分区 |
| 可注入 `Transport` / `EncodedAudioPort` | MQTT+UDP 传输扩展 |

Arduino 会递归编译库 `src/` 中的全部源码，因此这些可选模块不能只靠“不调用”来规避依赖；它们被彻底移出核心 `src/`。核心仍以 Opus 为线上格式，但不会强迫用户采用某个特定 Opus/I2S 库。

## 安装

需要：

- ESP32 Arduino Core 3.x；已验证基线为 3.3.11（ESP-IDF 5.5.5）。
- ArduinoJson 7.x（必需，Library Manager 会依据 `library.properties` 安装）。
- ArduinoWebsockets 0.5.4 或更新版本（只在使用内置 `ArduinoWebSocketTransport` 时安装）。

使用内置 WebSocket 适配器的 sketch 应像示例一样显式
`#include <ArduinoWebsockets.h>`。这让 Arduino 构建器发现可选依赖并把其 include
路径传给 Xiaozhi；不使用该适配器时则无需安装或包含 ArduinoWebsockets。

将本目录复制到 Arduino sketchbook 的 `libraries/Xiaozhi`，或者在 Arduino IDE 中选择“项目 → 加载库 → 添加 ZIP 库”。

显示库不是核心依赖，按需单独安装并打开对应示例：

- `examples/U8g2Display`：U8g2 2.36+，适合单色 OLED。
- `examples/TftEsPiDisplay`：TFT_eSPI 2.5+；烧录前配置其 `User_Setup`。
- `examples/LvglDisplay`：LVGL 9.x + TFT_eSPI 2.5+；LVGL 负责控件，TFT_eSPI 只负责刷屏。示例自带最小 `lv_conf.h`，干净安装无需修改 LVGL 库目录。该配置只启用示例实际使用的 Label 控件并关闭主题、复杂绘制、Flex/Grid；如需其他控件或圆角/阴影，请按需开启，并留意默认应用分区余量。

三个显示示例只是 UI 集成和独立安装验证，不包含麦克风、扬声器或 Opus 实现，也不会让显示库成为 Xiaozhi 的依赖。示例默认字体只保证 ASCII；显示中文 STT/TTS 时，应在对应显示库中选择覆盖中文字形的字体。

三个示例都只订阅 `Callbacks`，没有任何显示头文件进入本库 `src/`。因此一个项目不使用显示时，无需安装上述任一库。

## 最小用法

```cpp
#include <Xiaozhi.h>

xiaozhi::ArduinoWebSocketTransport transport;
xiaozhi::Client client(transport);

void setup() {
  // 先连接 Wi-Fi。服务端配置留空时直接使用小智官方配置服务。
  xiaozhi::ClientConfig config;
  xiaozhi::ProvisioningResult provisioning;
  std::string error;
  xiaozhi::OfficialServiceOptions serviceOptions;

  // 自建兼容配置服务时，解除下一行注释并修改地址：
  // serviceOptions.provisioning_url = "https://your-server.example/xiaozhi/ota/";

  if (!xiaozhi::ArduinoOfficialService::configure(
          config, provisioning, error, serviceOptions)) {
    Serial.println(error.c_str());
    return;
  }
  transport.setCACertificate(
      xiaozhi::ArduinoOfficialService::rootCACertificate());

  xiaozhi::Callbacks callbacks;
  callbacks.on_event = [](const xiaozhi::Event& event) {
    // 将 STT/TTS/emotion/alert 交给 Serial、LVGL 或任意 UI。
  };
  callbacks.on_error = [](xiaozhi::ErrorCode, const std::string& message) {
    Serial.println(message.c_str());
  };
  client.begin(config, callbacks);
}

void loop() {
  client.loop();
}
```

调用 `client.startListening()` 后建立会话。上行编码帧使用 `client.sendAudio()`；下行帧通过 `callbacks.on_audio` 返回。更推荐实现 `EncodedAudioPort` 并在 `begin()` 前调用 `client.attachAudioPort(&port)`，让音频扩展只处理 Opus 编解码和硬件队列。

为兼容 2.4.0 原实现，`protocol_version` 默认是 1。只有服务端明确支持时才设置为 2 或 3；三种版本的 JSON 语义相同，二进制 Opus 头格式不同。

## 并发契约

`Client::loop()` 是会话的单写者。自定义 `Transport` 应当从其 `loop()` 中派发回调，而不是从隐藏的 RTOS 任务直接调用。硬件中断和音频任务应先写入自己的有界队列，再由 `loop()` 交给 Client。状态机本身带锁，并在释放锁后通知监听器，避免重入死锁。

用户回调中不要直接调用 `startListening()`、`closeSession()`、`sendMcp()` 等控制 API；为防止递归状态转换，这些调用会返回失败。回调只设置一个 sketch 标志，再在 `client.loop()` 返回后执行控制动作。回调中调用 `end()` 会被安全延迟。

## 安全和资源限制

- `ClientConfig` 可以限制 JSON 和 Opus payload 大小，默认分别为 8192 和 4096 字节。
- v2/v3 二进制头使用显式大端读写，拒绝截断、伪造长度和超大 payload，不修改接收缓冲区。
- MCP 参数进行类型、必填、默认值和整数范围校验；错误文本由 ArduinoJson 转义。
- `user_only` MCP 工具默认拒绝远端调用；只有本地 UI 通过 `setUserToolAuthorizer()` 明确授权后才能执行。
- Provisioning HTTP 由调用者传入 `NetworkClient`。HTTPS 时应传入已配置 CA 的 `NetworkClientSecure`。
- 内置 WebSocket 适配器不提供关闭 TLS 证书校验的开关。

内置适配器在回调处还有 16384 字节硬上限。但 ArduinoWebsockets 0.5.x 会先聚合消息、再进入回调，因此这个检查不能阻止恶意服务端在依赖库内部进行首次分配。连接不可信端点时，应以 `_WS_CONFIG_MAX_MESSAGE_SIZE=16384` 构建 ArduinoWebsockets，或注入一个在读取阶段就限长的自定义 `Transport`。

## 当前兼容范围

核心目标为 ESP32、ESP32-S3、ESP32-C3、ESP32-C5、ESP32-C6 和 ESP32-P4。WebSocket 会话、协议、MCP、配置解析和已编码 Opus 帧接口属于本库；具体音频硬件、Opus 实现、离线唤醒、UI 和 MQTT+UDP 是独立扩展边界。详见 [MIGRATION.md](MIGRATION.md) 和 [TESTING.md](TESTING.md)。
