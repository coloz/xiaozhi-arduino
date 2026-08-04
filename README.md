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
| 会话状态机、事件和标准表情枚举 | LVGL/U8g2/屏幕 UI |
| WebSocket v1/v2/v3 编解码 | I2S 和具体音频 Codec 芯片 |
| hello/listen/abort/STT/TTS/LLM 消息 | PCM↔Opus 编解码器与重采样 |
| MCP 工具注册、参数校验和分页 | ESP-SR、WakeNet、VAD、AEC |
| OTA/激活配置解析与 HTTP 获取 | 摄像头、LED、按钮、电源管理 |
| ESP32 MAC、持久 UUID、固件版本 | 字体、表情、OGG 和 assets 分区 |
| 可注入 `Transport` / `EncodedAudioPort` | MQTT+UDP 传输扩展 |

Arduino 会递归编译库 `src/` 中的全部源码，因此这些可选模块不能只靠“不调用”来规避依赖；核心 `src/` 仍不包含可选音频硬件、Opus 或 ESP-SR 依赖。核心以 Opus 为线上格式，但不会强迫用户采用某个特定 Opus/I2S 库；完整示例在 sketch 翻译单元中按编译期 Profile 引入所选音频实现。

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
- `examples/U8g2RobotEyesEmotion`：U8g2RobotEyes 1.3+ 与 U8g2 2.36+；把服务端的精确表情字符串直接显示为 128×64 小智眼睛。
- `examples/TftEsPiDisplay`：TFT_eSPI 2.5+；烧录前配置其 `User_Setup`，并将 `Secrets.example.h` 复制为被 Git 忽略的 `Secrets.h` 后填写 Wi-Fi。
- `examples/TftEmotionFace`：TftRobotEyes 1.1+ 与 TFT_eSPI 2.5+；把服务端的精确表情字符串直接显示为全彩动画眼睛，并输出带稳定枚举值的串口状态。
- `examples/LvglDisplay`：LVGL 9.x + TFT_eSPI 2.5+；LVGL 负责控件，TFT_eSPI 只负责刷屏。示例自带最小 `lv_conf.h`，干净安装无需修改 LVGL 库目录。该配置只启用示例实际使用的 Label 控件并关闭主题、复杂绘制、Flex/Grid；如需其他控件或圆角/阴影，请按需开启，并留意默认应用分区余量。

五个显示示例都演示 UI 集成，但用途并不完全相同：`U8g2Display`、`U8g2RobotEyesEmotion` 和 `LvglDisplay` 是无音频的显示集成；`TftEsPiDisplay` 与 `TftEmotionFace` 是包含麦克风采集、Opus 编解码和扬声器播放的完整语音终端。所有显示头文件和这些可选音频依赖都留在示例侧，不会成为核心 `src/` 的依赖。示例默认字体只保证 ASCII；显示中文 STT/TTS 时，应在对应显示库中选择覆盖中文字形的字体。

两个完整语音示例通过编译期音频 Profile 选择硬件路径，只编译所选后端：

- ES8311 Codec 全双工；
- 麦克风与扬声器共享时钟的 I2S；
- 麦克风与扬声器使用独立 I2S 控制器，例如 INMP441 或 MSM261 搭配 MAX98357A；
- PDM 麦克风搭配 MAX98357A；
- 由应用提供回调的自定义 Codec。

Profile 选择、所需引脚和适用限制见 [AUDIO_PROFILES.md](AUDIO_PROFILES.md)。五个示例的显示层仍只通过 `Callbacks` 接收状态和事件，没有任何显示头文件进入本库 `src/`；不使用显示时，无需安装上述任一显示库。

## 表情事件

服务端的 `{"type":"llm","emotion":"happy"}` 会产生 `EventType::Emotion`。
核心将标准表情名解析到 `event.emotion_type`（`xiaozhi::Emotion`），同时在
`event.emotion` 中保留原始字符串，以兼容已有代码和自定义服务端表情：

```cpp
callbacks.on_event = [](const xiaozhi::Event& event) {
  if (event.type != xiaozhi::EventType::Emotion) return;

  Serial.printf("emotion=%s code=%u\n", event.emotion.c_str(),
                static_cast<unsigned>(event.emotion_type));
  switch (event.emotion_type) {
    case xiaozhi::Emotion::Happy:
      // 显示开心表情。
      break;
    case xiaozhi::Emotion::Sad:
      // 显示难过表情。
      break;
    case xiaozhi::Emotion::Unknown:
      // event.emotion 仍保存服务端的自定义名称。
      break;
    default:
      break;
  }
};
```

`emotionFromName()` 和 `emotionName()` 可用于应用自己的字符串/枚举双向转换。
枚举数值固定，可用于本地串口协议；线上 Xiaozhi 协议仍使用小写字符串名称。
标准枚举按官方对照表提供 21 种：`neutral`、`happy`、`laughing`、`funny`、
`sad`、`angry`、`crying`、`loving`、`embarrassed`、`surprised`、`shocked`、
`thinking`、`winking`、`cool`、`relaxed`、`delicious`、`kissy`、`confident`、
`sleepy`、`silly`、`confused`。

`TftEmotionFace` 和 `U8g2RobotEyesEmotion` 都直接调用
`eyes.setExpression(event.emotion.c_str())`，不再维护第二份表情映射表。两者使用官方配置服务；将各自的 `Secrets.example.h` 复制为被 Git 忽略的 `Secrets.h` 并填写 Wi-Fi 即可。没有该文件时，示例自动离线轮播全部 21 种表情，便于先做屏幕和动画板测。

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

`EncodedAudioPort` 还提供可选的移动播放、`cancelPlayback()` 和 `queuedPlaybackMs()` 钩子；旧扩展无需修改即可继续编译。实现取消钩子后，用户打断、TTS 新轮次、会话关闭或网络断开会立即废弃旧播放数据，避免缓冲语音继续播出。

完整的 `TftEsPiDisplay` 音频示例将 I2S 采集、Opus 编解码和 I2S 输出放在独立任务中，`loop()` 只限量转交已编码的上行包，并让上行先于一次可能批量处理消息的 WebSocket 轮询。四条热路径队列使用预分配固定环，PCM/Opus 缓冲池复用；采集下采样使用按实际比例生成的 15-tap Q15 滤波器，48→24 kHz 播放使用流式 31-tap Q15 滤波器，上采样采用跨包连续的单样本延迟因果插值。上下行队列有时延上限并丢弃最旧数据，默认下行等待上限由 2400 ms 收紧为 600 ms。采集、唤醒、上行和播放均用代际号阻断旧会话数据，用户打断会先本地静音并等播放尾音消失后再开麦。会话仍连接时示例关闭 Wi-Fi 省电，以降低首包和连续对话抖动。这些策略针对 ESP32-S3 完整音频示例；核心库仍不隐式启用 AFE/AEC，是否使用服务端 AEC 或板级 ESP-SR AFE 应按硬件回声路径单独验证。

为兼容 2.4.0 原实现，`protocol_version` 默认是 1。只有服务端明确支持时才设置为 2 或 3；三种版本的 JSON 语义相同，二进制 Opus 头格式不同。`enable_server_aec` 只允许与协议 v2 一起使用；此时完整音频端口会把“已实际写入 I2S 的下行包时间戳”配给后续上行帧，`toggleChat()` 与唤醒入口默认使用 `Realtime`。未启用服务端 AEC 时，上行线协议时间戳固定为 0。

## 并发契约

`Client::loop()` 是会话的单写者。自定义 `Transport` 可以从 `connect/send/close` 同步派发回调，也可以从其 `loop()` 派发，但所有调用必须在 Client 所在的同一任务，不能从隐藏的 RTOS 任务直接进入 Client。回调产生的协议文本会先进入小型有界队列，在当前 `connect/loop/send` 返回后发送，因此 Transport 不必支持递归 `send`；同步回调链超过上限会关闭会话，不能无限自激。`sendText/sendBinary` 返回前必须消费或复制输入数据，不能保留指针；`close()` 必须幂等、即使 `connected()==false` 也清除旧回调，并允许从同步回调请求（实现可延迟到当前调用栈退出后再销毁对象）。硬件中断和音频任务应先写入自己的有界队列，再由音频端口的 `loop()` 交给 Client；音频端口的 `end()` 返回前必须停止工作任务和全部上行回调。状态机本身带锁，并在释放锁后通知监听器，避免重入死锁。

用户回调中不要直接调用 `startListening()`、`closeSession()`、`sendMcp()` 等控制 API；为防止递归状态转换，这些调用会返回失败。回调只设置一个 sketch 标志，再在 `client.loop()` 返回后执行控制动作。回调中调用 `end()` 会被安全延迟。

## 安全和资源限制

- `ClientConfig` 可以限制 JSON 和 Opus payload 大小，默认分别为 8192 和 4096 字节。
- v2/v3 二进制头使用显式大端读写，拒绝截断、伪造长度和超大 payload，不修改接收缓冲区。
- MCP 参数进行类型、必填、默认值和整数范围校验；错误文本由 ArduinoJson 转义。
- `user_only` MCP 工具默认拒绝远端调用；只有本地 UI 通过 `setUserToolAuthorizer()` 明确授权后才能执行。
- Provisioning HTTP 由调用者传入 `NetworkClient`。HTTPS 时应传入已配置 CA 的 `NetworkClientSecure`。
- 内置 WebSocket 适配器不提供关闭 TLS 证书校验的开关。

内置适配器把 ArduinoWebsockets 0.5.x 设为逐分片通知，并用总长 16384 字节的缓冲区自行重组；超过上限或分片顺序错误会立即关闭连接，因此不会让依赖库无限聚合整条消息。ArduinoWebsockets 0.5.4 的策略 setter 不会重建已有 StreamBuilder，所以每个新 client 的首条分片消息仍会在依赖内短暂保留第二份；单个 WebSocket 帧也仍由依赖库在回调前分配。连接不可信端点时，应以 `_WS_CONFIG_MAX_MESSAGE_SIZE=16384` 构建 ArduinoWebsockets，或注入一个在读取阶段就限长的自定义 `Transport`。

## 当前兼容范围

核心目标为 ESP32、ESP32-S3、ESP32-C3、ESP32-C5、ESP32-C6 和 ESP32-P4。WebSocket 会话、协议、MCP、配置解析和已编码 Opus 帧接口属于本库；具体音频硬件、Opus 实现、离线唤醒、UI 和 MQTT+UDP 是独立扩展边界。详见 [MIGRATION.md](MIGRATION.md) 和 [TESTING.md](TESTING.md)。
