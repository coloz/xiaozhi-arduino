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
| MCP 工具注册、参数校验、分页和服务端 AEC 协议 | ESP-SR、WakeNet、VAD、设备端 AFE/AEC |
| OTA/激活配置解析与 HTTP 获取 | 摄像头、LED、按钮、电源管理 |
| ESP32 MAC、持久 UUID、固件版本 | 字体、表情、OGG 和 assets 分区 |
| 可注入 `Transport` / `EncodedAudioPort` | MQTT+UDP 传输扩展 |

Arduino 会递归编译库 `src/` 中的 `.cpp`，因此可选音频实现以按需包含的头文件保存在 `src/xiaozhi/audio`。普通应用不定义 `XIAOZHI_AUDIO_BOARD` 时不会引入 Opus、Codec 或 ESP-SR 依赖；定义后由 `Xiaozhi.h` 在 sketch 翻译单元中引入所选音频实现。

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
- `examples/TftEsPiDisplay`：TFT_eSPI 2.5+；烧录前配置其 `User_Setup`，并直接在 `.ino` 顶部填写 Wi-Fi。
- `examples/TftEmotionFace`：TftRobotEyes 1.1+ 与 TFT_eSPI 2.5+；把服务端的精确表情字符串直接显示为全彩动画眼睛，并输出带稳定枚举值的串口状态。
- `examples/LvglDisplay`：LVGL 9.x + TFT_eSPI 2.5+；LVGL 负责控件，TFT_eSPI 只负责刷屏。示例自带最小 `lv_conf.h`，干净安装无需修改 LVGL 库目录。该配置只启用示例实际使用的 Label 控件并关闭主题、复杂绘制、Flex/Grid；如需其他控件或圆角/阴影，请按需开启，并留意默认应用分区余量。

五个显示示例都演示 UI 集成，但用途并不完全相同：`U8g2Display`、`U8g2RobotEyesEmotion` 和 `LvglDisplay` 是无音频的显示集成；`TftEsPiDisplay` 与 `TftEmotionFace` 是包含麦克风采集、Opus 编解码和扬声器播放的完整语音终端。显示头文件仍只留在示例侧；可选音频实现位于库的 `src/xiaozhi/audio`，仅在选择音频板时引入。示例默认字体只保证 ASCII；显示中文 STT/TTS 时，应在对应显示库中选择覆盖中文字形的字体。

两个完整语音示例通过编译期音频 Profile 选择硬件路径，只编译所选后端：

- ES8311 Codec 全双工；
- 麦克风与扬声器共享时钟的 I2S；
- 麦克风与扬声器使用独立 I2S 控制器，例如 INMP441 或 MSM261 搭配 MAX98357A；
- PDM 麦克风搭配 MAX98357A；
- 由应用提供回调的自定义 Codec。

Profile 选择、所需引脚和适用限制见 [AUDIO_PROFILES.md](AUDIO_PROFILES.md)。五个示例的显示层仍只通过 `Callbacks` 接收状态和事件，没有任何显示头文件进入本库 `src/`；不使用显示时，无需安装上述任一显示库。

## 音频开发板预设

常用开发板的麦克风、扬声器、Codec、I2S 和音频 I2C 配置保存在
`src/xiaozhi/boards`。在 `.ino` 开头、包含 `Xiaozhi.h` 之前选择一个宏值
即可，无需创建 `BoardConfig.h` 或逐项复制音频引脚：

```cpp
#define XIAOZHI_AUDIO_BOARD XIAOZHI_AUDIO_BOARD_NULLLAB_AI_VOX
#include <Xiaozhi.h>
```

也可以通过构建参数选择，例如
`-DXIAOZHI_AUDIO_BOARD=XIAOZHI_AUDIO_BOARD_NULLLAB_AI_VOX`。当前内置：

| 选择值 | 开发板 | 音频硬件 |
| --- | --- | --- |
| `XIAOZHI_AUDIO_BOARD_OJ_ESP32S3_BASIC` | OpenJumper ESP32 AIOT Basic（`oj_esp32s3basic`） | ES8311，共享全双工 I2S |
| `XIAOZHI_AUDIO_BOARD_NULLLAB_AI_VOX` | NULLLAB AI VOX 一代 | SPH0645 麦克风 + 独立 I2S 功放 |

完整示例默认显式选择 `XIAOZHI_AUDIO_BOARD_OJ_ESP32S3_BASIC`。
AI VOX 预设对应 `ai_vox` 仓库的 `examples/ai_vox_board`，不是引脚不同的
AI-VOX3。

音频预设不会定义任何屏幕、背光或按钮引脚。TFT_eSPI 不支持构造函数传入
引脚，因此由用户配置已安装库的 `User_Setup.h` 或构建参数，示例目录不再
附带显示配置头文件；U8g2 的时钟和数据引脚直接保留在单个 `.ino` 的构造器
附近。切换音频板不会改变显示配置。

自定义音频板使用 `XIAOZHI_AUDIO_BOARD_CUSTOM`，并在包含
`xiaozhi/boards/BoardPresets.h` 前定义所需的 `BOARD_AUDIO_*` 与
`XIAOZHI_AUDIO_PROFILE` 宏。

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
xiaozhi::ClientRuntime runtime(client);

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
  if (runtime.begin(config, callbacks)) {
    runtime.requestStartListening(xiaozhi::ListeningMode::AutoStop);
  }
}

void loop() {
  // 这里只派发用户回调；WebSocket、协议超时和音频上行在独立任务运行。
  runtime.loop();
}
```

推荐 Arduino 应用使用 `ClientRuntime`。它把单写者 `Client`、WebSocket 轮询、协议超时和 `EncodedAudioPort::loop()` 固定在独立 FreeRTOS 任务；`requestStartListening()`、`requestToggleChat()`、`requestWakeWordDetected()` 等请求通过有界队列非阻塞提交。显示刷新、传感器读取或第三方库偶尔卡住 Arduino `loopTask` 时，小智链路仍继续运行。

`runtime.loop()` 只限量派发用户回调，默认每次最多 4 个；即使暂时不调用，也只会延迟 UI/日志，不会停止协议和已挂接音频端口。回调队列满时会丢弃新回调而不阻塞实时任务，可用 `runtime.stats()` 查看命令/回调丢弃数和队列最高水位。不要在 Runtime 活动期间直接调用底层 `client`；改用 `request*` 方法。需要完全自定义调度或同步主机测试时，仍可不创建 Runtime，继续直接使用 `Client::loop()`。

默认 Runtime 使用 8192 字节栈、优先级 2、固定到 core 0；活动会话每 2 ms 服务一次，空闲时降到 20 ms，命令仍会立即唤醒任务。可通过 `ClientRuntimeConfig` 调整。双核 Arduino 通常把 `loopTask` 放在 core 1，因此默认值能隔离大多数用户代码；单核芯片的有效 core 仍是 0。回调消息按需分配，设置 `on_audio` 会为了跨任务派发复制 Opus payload；已经挂接 `EncodedAudioPort` 且不需要观察原始下行包时，省略 `on_audio` 可减少热路径分配。

更推荐实现 `EncodedAudioPort` 并在 `runtime.begin()` 前调用 `client.attachAudioPort(&port)`，让音频扩展处理 Opus 编解码和硬件队列。未使用音频端口的高级用法可直接用 `Client` 的同步 API 管理手工上行帧。

`EncodedAudioPort` 还提供可选的移动播放、`cancelPlayback()` 和 `queuedPlaybackMs()` 钩子；旧扩展无需修改即可继续编译。实现取消钩子后，用户打断、TTS 新轮次、会话关闭或网络断开会立即废弃旧播放数据，避免缓冲语音继续播出。

完整的 `TftEsPiDisplay` 音频示例将 I2S 采集、Opus 编解码和 I2S 输出放在独立任务中，Runtime 任务限量批量转交已编码的上行包，并让上行先于一次可能批量处理消息的 WebSocket 轮询。四条热路径队列使用预分配固定环，PCM 缓冲池复用；Opus 编码只保留一个最大尺寸临时缓冲，排队包按实际编码长度占用内存。采集下采样使用按实际比例生成的 15-tap Q15 滤波器，48→24 kHz 播放使用流式 31-tap Q15 滤波器，上采样采用跨包连续的单样本延迟因果插值。上下行压缩包队列默认与官方 2.4.0 一致保留最多 2400 ms，既吸收 Wi-Fi/服务器突发，又在用户手动打断时由代际号立即废弃全部旧包。会话仍连接时示例关闭 Wi-Fi 省电，以降低首包和连续对话抖动。设备端 ESP-SR AFE/AEC 仍是板级扩展，必须按真实回声参考路径单独实现和验证。

为兼容 2.4.0 原实现，`protocol_version` 默认是 1。核心 `ClientConfig` 默认请求服务端 AEC/语音打断，但完整 TFT 示例只在服务端实际下发协议 v2 时启用 `Realtime`：v2 会把“已实际写入 I2S 的下行包时间戳”配给上行帧。v1/v3 没有该字段，实体测试会把扬声器回声误判为插话、造成回复只播几个字，因此示例自动回退 `AutoStop`，保证回复完整；BOOT 键、串口 `t` 和 `abortSpeaking()` 仍可立即手动打断。

完整 TFT 示例可直接在 `.ino` 中用编译期宏配置：

```cpp
#define XIAOZHI_ENABLE_SERVER_AEC_DEFAULT 1
#define XIAOZHI_ENABLE_VOICE_BARGE_IN_DEFAULT 1
#define XIAOZHI_ALLOW_UNTIMESTAMPED_SERVER_AEC 0
```

运行期可逐个 Client 覆盖：

```cpp
xiaozhi::ClientConfig config;
config.enable_server_aec = true;
config.enable_voice_barge_in = true;
```

将两项都设为 `false` 即回到 `AutoStop` 半双工。只有确认自定义 v1/v3 服务端或设备端确实能消除回声时，才将 `XIAOZHI_ALLOW_UNTIMESTAMPED_SERVER_AEC` 设为 1；官方 v1 端点不应强制开启。语音打断要求有效 AEC；手动打断不受这些开关影响。

## 并发契约

使用 `ClientRuntime` 时，底层 `Client` 只由 Runtime 任务访问。Arduino 任务和用户回调只能调用 `runtime.request*()`、读取 Runtime 的原子状态快照或调用 `runtime.loop()`；不要混用 `client.loop()` 或其他 Client 控制 API。`runtime.end()` 会等待服务任务退出并完成 Client/音频端口清理；若有限超时返回 `false`，对象仍保持有效，应稍后重试，不能提前销毁 Transport、Client 或音频端口。

`Client::loop()` 是会话的单写者。自定义 `Transport` 可以从 `connect/send/close` 同步派发回调，也可以从其 `loop()` 派发，但所有调用必须在 Client 所在的同一任务，不能从隐藏的 RTOS 任务直接进入 Client。回调产生的协议文本会先进入小型有界队列，在当前 `connect/loop/send` 返回后发送，因此 Transport 不必支持递归 `send`；同步回调链超过上限会关闭会话，不能无限自激。`sendText/sendBinary` 返回前必须消费或复制输入数据，不能保留指针；`close()` 必须幂等、即使 `connected()==false` 也清除旧回调，并允许从同步回调请求（实现可延迟到当前调用栈退出后再销毁对象）。硬件中断和音频任务应先写入自己的有界队列，再由音频端口的 `loop()` 交给 Client；音频端口的 `end()` 返回前必须停止工作任务和全部上行回调。状态机本身带锁，并在释放锁后通知监听器，避免重入死锁。

直接使用 Client 时，用户回调中不要调用 `startListening()`、`closeSession()`、`sendMcp()` 等控制 API；为防止递归状态转换，这些调用会返回失败。可以设置 sketch 标志并在 `client.loop()` 返回后执行。使用 Runtime 时则可从回调提交 `runtime.request*()`。

## 安全和资源限制

- `ClientConfig` 可以限制 JSON 和 Opus payload 大小，默认分别为 8192 和 4096 字节。
- v2/v3 二进制头使用显式大端读写，拒绝截断、伪造长度和超大 payload，不修改接收缓冲区。
- MCP 参数进行类型、必填、默认值和整数范围校验；错误文本由 ArduinoJson 转义。
- `user_only` MCP 工具默认拒绝远端调用；只有本地 UI 通过 `setUserToolAuthorizer()` 明确授权后才能执行。
- Provisioning HTTP 由调用者传入 `NetworkClient`。HTTPS 时应传入已配置 CA 的 `NetworkClientSecure`。
- 内置 WebSocket 适配器不提供关闭 TLS 证书校验的开关。

内置适配器把 ArduinoWebsockets 0.5.x 设为逐分片通知，并用总长 16384 字节的缓冲区自行重组；超过上限或分片顺序错误会立即关闭连接，因此不会让依赖库无限聚合整条消息。ArduinoWebsockets 0.5.4 的策略 setter 不会重建已有 StreamBuilder，所以每个新 client 的首条分片消息仍会在依赖内短暂保留第二份；单个 WebSocket 帧也仍由依赖库在回调前分配。连接不可信端点时，应以 `_WS_CONFIG_MAX_MESSAGE_SIZE=16384` 构建 ArduinoWebsockets，或注入一个在读取阶段就限长的自定义 `Transport`。

## 当前兼容范围

核心目标为 ESP32、ESP32-S3、ESP32-C3、ESP32-C5、ESP32-C6 和 ESP32-P4。WebSocket 会话、协议、MCP、配置解析和已编码 Opus 帧接口属于本库；具体音频硬件、Opus 实现、离线唤醒、UI 和 MQTT+UDP 是独立扩展边界。详见 [PERFORMANCE.md](PERFORMANCE.md)、[MIGRATION.md](MIGRATION.md) 和 [TESTING.md](TESTING.md)。
