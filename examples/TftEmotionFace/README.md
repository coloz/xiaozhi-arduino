# TftEmotionFace

该示例在 `grokEyes` 表情界面上整合了 Xiaozhi 官方服务、注册引导、
编译期可选音频硬件、Opus 编解码、可选 WakeNet 唤醒和 BOOT 键会话控制。

## 硬件与编译设置

- ESP32-S3，16 MB Flash，8 MB OPI PSRAM
- 分区：`ESP SR 16M`（`PartitionScheme=esp_sr_16`）
- USB CDC On Boot：Enabled，便于查看串口诊断
- 开发板音频接线由 `.ino` 开头的 `XIAOZHI_BOARD` 直接选择，不需要
  `BoardConfig.h`；完整选择方法见项目根目录的
  `AUDIO_PROFILES.md`
- TFT_eSPI 不支持构造函数传入引脚；请在已安装库的 `User_Setup.h` 或构建
  参数中配置显示控制器和引脚；OJoy 可使用下面的专用构建脚本

## OJoy 圆屏开发板

`build_ojoy.ps1` 选择 `OJ_ESP32S3_OJOY` 音频预设，复用 grokEyes 的
CH13613 480×480 QSPI 刷新实现。需要支持 `CH13613_DRIVER` 的本地 TFT_eSPI
分支（默认 `D:\Git\TFT_eSPI`）和 grokEyes（默认 `D:\Git\grokEyes`）；
普通 TFT_eSPI 2.5.43 不包含此驱动。脚本把 `ojoy_tft_setup.h` 应用于整个构建，
确保示例、TFT_eSPI 和 grokEyes 的显示配置一致。
注册和错误提示复用 grokEyes 的画布整帧推送，满足 CH13613 的 2×2 窗口对齐要求；
提示文字留出圆屏上下边缘空间，不额外分配全屏缓冲。

先把 `Secrets.example.h` 复制为 `Secrets.h` 并填写 Wi-Fi；`Secrets.h` 已被 Git 忽略。
从项目根目录执行：

```powershell
# 仅编译：16 MB Flash、8 MB OPI PSRAM、ESP-SR 分区、USB CDC
powershell -ExecutionPolicy Bypass -File examples/TftEmotionFace/build_ojoy.ps1

# 编译并烧录，脚本自动校验并替换“你好小智”模型
powershell -ExecutionPolicy Bypass -File examples/TftEmotionFace/build_ojoy.ps1 -Upload -Port COM39

# 依赖库在其他目录时
powershell -ExecutionPolicy Bypass -File examples/TftEmotionFace/build_ojoy.ps1 -GrokEyesLibrary D:\Git\grokEyes -TftLibrary D:\Git\TFT_eSPI -Upload -Port COM39

# 串口验收，触发一次对话；日志保存在 .build/ojoy-logs/
powershell -ExecutionPolicy Bypass -File examples/TftEmotionFace/verify_ojoy.ps1 -Port COM39 -ToggleChat

# 注入唤醒事件，检查从 Speaking 打断并回到 Listening 的控制链路
powershell -ExecutionPolicy Bypass -File examples/TftEmotionFace/verify_ojoy.ps1 -Port COM39 -WakeInterrupt -Seconds 60
```

音频预设使用 ES8311 `0x18`、24 kHz/16 位共享全双工 I2S，功放由 Codec 后端控制。
引脚来自 OJoy 引脚表，并与本地 `ojoy_es8311` 板级初始化核对：

| 功能 | GPIO |
| --- | --- |
| ES8311 / RTC / IMU / 电量计 Wire SDA、SCL | 5、4 |
| I2S MCLK、BCLK、WS | 6、14、12 |
| 播放 TX → Codec DIN、采集 RX ← Codec DOUT | 11、13 |
| 功放 CTRL（高有效） | 9 |
| CH13613 SCLK、CS、RST、TE | 17、45、7、38 |
| CH13613 D0、D1、D2、D3 | 8、16、15、18 |
| BOOT（本示例会话按钮）、BTN | 0、46 |
| 触摸 Wire1 SDA、SCL、INT、RST | 21、47、10、48 |
| SD CMD、CLK、D0、D1、D2、D3 | 1、2、40、39、42、41 |
| 马达、RTC_INT / UART RX、UART TX | 3、44、43 |
| USB D−、D+ | 19、20 |

本示例启用屏幕、音频和 BOOT 会话控制；触摸、SD、RTC、IMU、电量计和马达不在本示例中初始化。
串口使用 115200；启动日志报告 480×480 显示分配和 ES8311 探测结果，
运行日志的 `frames` 应持续增加。发送 `t` 或按 BOOT 测试会话；
未注册时根据屏幕/串口激活码在 `xiaozhi.me` 完成绑定后自动重启重试。
帧数和音频包日志用于软件验收，屏幕实际显示、麦克风及扬声器效果还需现场观察。

## 唤醒模型与通用命令

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
- 默认支持唤醒词打断：播报过程中再次说“你好小智”，本地 WakeNet 检测后立即
  清除旧播放队列，发送 `wake_word_detected` 原因的中止请求，再开启新一轮收音。
  该功能使用 AutoStop 模式，不依赖服务端 AEC 或 WebSocket 时间戳，协议 v1 也可使用。
- 按一下 BOOT 键也可开始或结束对话。
- 串口发送 `t` 可执行与 BOOT 键相同的会话切换，波特率为 115200。
- 串口发送 `w` 可注入唤醒事件，验证打断控制链路；实际唤醒词检测需对设备说话验证。
- 服务器表情事件会输出为 `EMOTION:<enum>,<name>`，同时交给
  `grokEyes` 显示；名称严格使用 Xiaozhi 的 21 个标准字符串。
- 未注册时屏幕显示 `xiaozhi.me` 和激活码，并自动刷新注册状态。
- ES8311 Profile 找不到 Codec，或任一 Profile 的 I2S/PDM 引脚、控制器、
  采样率/位宽不合法时，屏幕显示 `AUDIO ERROR`，详细原因输出到串口。

默认 `XIAOZHI_ENABLE_WAKE_WORD_INTERRUPTION_DEFAULT=1`、
`XIAOZHI_ENABLE_VOICE_BARGE_IN_DEFAULT=0`。如需服务端 Realtime 连续语音打断，
将前者设为 `0`、后者设为 `1`，并使用支持 AEC 时间戳的协议；
本地 WakeNet 和 Realtime 上传不能同时直接读取同一个 I2S RX 数据流。
OJoy 使用一个模拟麦克风。默认启用 `config.enableWakeAec=true`：ES8311 左声道为麦克风，右声道为
同步 DAC 参考；分别重采样到 16 kHz，以 `MR` 格式送入 ESP-SR AFE，
由 AFE 执行本地 AEC 和内置 WakeNet 检测（`DET_MODE_95`）。
该路径只用于唤醒，采用 `AFE_TYPE_SR` / `AEC_MODE_SR_HIGH_PERF`；
通话用的 Opus 编码模式与上行语音处理不变。
两个 I2S slot 并不代表两个麦克风。`wakeAecDuringPlaybackOnly=true` 让待机绕过 AEC，
播报及本地播放尾音期间启用 AEC；屏幕和串口分别显示 `AEC OFF` 和 `aec_on=0/1`。
右声道不能被 `CaptureChannel::Auto` 选成麦克风。此功能要求 PSRAM，与服务端
AEC 开关独立；其他开发板只有提供相同的同步参考输入时才能启用。
OJoy 示例将 `config.hardware.es8311.outputVolumeDb` 设为 `-12.0f`，
低于 [esp_codec_dev 默认音量曲线](https://github.com/espressif/esp-adf/blob/master/components/esp_codec_dev/esp_codec_dev.c)
的 100%（0 dB），给回声消除留出余量；正常麦克风增益为 24 dB。
板级预设仍为 −30 dB，示例在 `makeAudioConfig()` 中覆盖。
播报时启用 `config.hardware.es8311.playbackMicrophoneGainDb=12.0f`：
扬声器取消静音前将 ES8311 麦克风前级降至 12 dB，静音后恢复 24 dB，为 ADC
留出输入余量。队列排空后的静音延时为 150 ms（默认 I2S DMA 最多约 60 ms），
让下一轮收音及时恢复正常增益。串口 `mic_db` 显示当前增益，`clip` 累计唤醒采样中
峰值达到 32,760 及以上的读取次数；一次读取为 10 ms，不代表削顶样本总数。

对照本地 `xiaozhi-esp32-2.4.2` 的实现：`Application::HandleStateChangedEvent()`
在 Speaking 状态保留 AFE 唤醒，`AfeAudioEngine::ApplyAfeControls()` 在有参考通道且
唤醒开启时启用 AEC；`Application::HandleWakeWordDetectedEvent()` 检测成功后执行
`AbortSpeaking(kAbortReasonWakeWordDetected)` 并重新收音。最大音量下能否识别，
还取决于送入 AEC 的音频质量。原项目的普通 `Es8311AudioCodec` 为单通道、无参考、
固定 30 dB 输入增益，`SetOutputVolume()` 只设置输出音量，没有随音量调整麦克风
增益的逻辑，不能直接照搬到 OJoy 的 `MR` 配置。本示例的播报增益切换是 OJoy 适配。

[Espressif 麦克风设计指南](https://docs.espressif.com/projects/esp-sr/en/latest/esp32s3/audio_front_end/Espressif_Microphone_Design_Guidelines.html)
要求最大音量下回采不饱和，并约束功放失真和麦克风处的声压。实机原配置
（输出 0 dB、输入 24 dB）已记录到麦克风峰值 32,768、RMS 28,338；
削顶后的 PCM 无法靠后级调低数字增益恢复。降低模拟增益仍需现场验证：
检查播报时 `mic_db=12`、静音后 `mic_db=24`、`clip` 是否继续增长，并测试真实唤醒词
打断。若麦克风或扬声器本身已失真，仅调整 ADC 增益不能保证最大音量下可靠打断。
串口 `w` 注入只能验证中止和重新收音的控制流程，不能代替这一声学验收。

示例将 `config.wakeDetectionThreshold` 设为 `0.55`；模型文件的默认阈值约为
`0.63`。这偏向首遍命中，也可能增加误唤醒；库默认值 `0` 保留模型阈值，
可手动设置范围为 `0.4`～`0.9999`。应同时测试安静待机、正常对话和播报打断。
示例关闭 Wi-Fi 省电以减少网络响应抖动，上行发送池匹配每批四包的音频输出。
示例让 1 KiB 及以上的普通堆分配优先使用 PSRAM，给 WakeNet 任务栈和 Wi-Fi/TLS
保留内部内存；I2S DMA 仍使用驱动要求的内存。仅有 PSRAM 容量并不保证内部堆足够。

实现参考 `xiaozhi-esp32-2.4.2` 的 `Application::HandleWakeWordDetectedEvent()`、
`Application::HandleStateChangedEvent()` 和 `AfeAudioEngine::ApplyAfeControls()`：
播报时持续进行带回声消除的唤醒检测，命中后中止旧回复并重新收音。
本示例沿用音频任务及 RX 互斥喂入 AFE，独立的结果任务通过有超时的
`fetch_with_delay` 等待唤醒结果；仅采样任务读取 I2S。AFE 重置和检测控制由结果任务
执行，并与 feed 互斥，避免跨任务重置处理中的缓冲。
服务器先结束 TTS 而本地队列仍在播放时继续检测，
直到上行采集真正接管 RX。

`[wake-status]` 中 `frames` / `aec` 是累计处理帧数，`rms` 是原始麦克风电平，
`ref` 是播放参考电平，`clean` 是消除回声后的电平；播放时应保持处理帧数增长。
串口 `w` 不经过麦克风或 AEC，不能用它代替真实人声验收。

## 调试状态与延迟

`XIAOZHI_SHOW_DEBUG_STATUS` 默认开启，在圆屏底部显示 Wi-Fi、会话状态、
`WAKE ON/OFF`、累计 `HITS` 和音频电平。命中后立即显示 `HEARD!`，持续约
6 秒；这表示本地检测成功，开始播放云端回复还要经过网络和服务端处理。
`MIC SEND` 表示正在上传收音，此时 MIC 显示实时采集电平；REF/AFE 是上次唤醒处理值。
Listening 状态显示 `LISTENING: SPEAK NOW`，已经可以直接说话，不需要再次唤醒。
服务端关闭已建立的会话后返回 Idle，重新打开本地唤醒检测；示例通过
`AsyncTransportConfig::reconnect_after_disconnect=false` 避免自动重开已结束的聊天。
库默认仍允许断线重连。

真实命中时串口输出 `dispatch_ms`、`capture_ms`、`first_downlink_ms`，均从本地
检测时间计起。它们用于区分事件/UI 调度、开始收音和首个下行音频包的延迟；
不等同于从说话开始计算的声学识别耗时，也不等同于首个声音的实际播放时间。

如需本地音频诊断，构建时增加 `-WakeDiagnostics`。串口 `r` 开始循环记录，保留最近
40 秒送入 AFE 的双声道 PCM 和处理后的单声道 PCM，`s` 停止，`d` 导出。
平时不录音。导出时暂停眼睛动画、显示导出状态，音频任务继续运行。

```powershell
powershell -ExecutionPolicy Bypass -File examples/TftEmotionFace/build_ojoy.ps1 -WakeDiagnostics -Upload -Port COM39
# 下一次播报时开始记录，随后自动导出；不要同时打开其他串口监视器
powershell -ExecutionPolicy Bypass -File examples/TftEmotionFace/capture_wake_ojoy.ps1 -Port COM39
# 待机漏检比较：一直等到真实唤醒再导出，保留命中前最近 40 秒
powershell -ExecutionPolicy Bypass -File examples/TftEmotionFace/capture_wake_ojoy.ps1 -Port COM39 -RecordNow -WaitForWake -Seconds 3600
```

PCM 和日志保存在本地 `.build/ojoy-diagnostics/`，不提交到仓库。麦克风与播放参考
交错存入 `mic-reference.pcm`，AFE 输出存入 `afe-output.pcm`，均为 16 kHz PCM16LE。
测试可能始终未命中时，可附加 `-StopFile <尚不存在的文件路径>`；现场测试结束后
创建该文件，脚本会停止记录并导出，无需占用第二个串口连接。

## OJoy 实机记录（2026-09-14～15）

- COM23，Arduino-ESP32 3.3.11，16 MB Flash / 8 MB OPI PSRAM，固件和唤醒模型烧录校验通过。
- 串口确认 CH13613 480×480、ES8311 `0x18` 和 WakeNet 模型初始化成功。
- Wi-Fi 联网、账号绑定、STT、TTS 和表情切换已验证，现场确认眼睛动画与扬声器回复正常。
- `-WakeInterrupt` 测试通过：在 Speaking 状态注入唤醒事件，接受请求后切回 Listening 并开启新一轮会话。
- 后续 90 秒连续运行中帧数持续增长，采集/编码/解码/输出错误计数均为 0。
- 现场复测确认旧版空闲可唤醒、播报无法打断。诊断发现 WakeNet 持续处理且无丢帧，
  但麦克风在播放中反复达到 32768 峰值，RMS 达 1.4 万～2.5 万；旧版没有本地 AEC。
- 调整增益和 PSRAM 分配后，启动、采样与播报正常，内部堆最低约 30 KB；
  手工组合 AEC + 独立 WakeNet 的现场复测仍未通过播报打断。
- 后续改用 AFE 内置 WakeNet，COM39 的替换设备已烧录并完成绑定。
- COM39 已捕获到一次真实播报中唤醒，现场确认可能成功一次；用户反馈在约 5 cm
  距离仍经常漏检。状态栏版本可唤醒并回复，但有时需要说两遍。不能将控制链路
  通过视为稳定声学打断通过。
- 首遍唤醒优化调整了增益、判定阈值、网络省电和屏幕命中反馈；可靠性须以该版本
  的真实人声复测结果为准。
- 0.55 阈值、24/−24 dB 版本在 COM39 实测捕获到待机和播报中真实命中。待机命中后
  46 ms 分派到 UI、764 ms 开始收音、1755 ms 收到首个下行音频包；播报命中后
  87 ms 分派到 UI、180 ms 重新收音、1516 ms 收到首个下行包。
  用户确认播报打断正常；待机曾有首遍成功，但后续仍反馈需要两遍，首遍漏检尚未完成验收。
- 状态提示版占 1,956,779 字节（62%），完整编译、关闭 ESP-SR 的语法检查、
  COM39 固件/模型烧录校验通过。烧录后连续 30 秒检查中 Wi-Fi 在线、AFE 和显示
  帧数持续增长，无启动错误或重启。`HEARD!` 提示已延长到 6 秒。
- 对照 `xiaozhi-esp32-2.4.2`：原版 ES8311 的逻辑输入为一个麦克风、无参考输入；
  AFE 使用 `M`，不启用 AEC。OJoy 的 `MR` 为一个模拟麦克风加内部 DAC 参考。
  本地受控回放使用相同 16 kHz 合成语音、0.55 阈值和实时喂入速度；各 AFE 管线均
  处理了 184,832 个样本。常开 AEC 的两个语音检测点为 3,264/8,064 ms，
  待机绕过 AEC 和原版 `M` 管线均为 2,880/7,680 ms，本样本提前 384 ms。
  合成输入开头的全零静音也出现过误触发，因此只比较两个真实语音段的检测点，
  不据此计算成功率或进一步降低阈值。该测试不能代替现场首遍唤醒验收。
- 9 月 15 日，待机单麦旁路版本（1,957,291 字节）已在 COM39 完整烧录并通过
  应用及模型校验。30 秒检查确认联网、`microphones=1`、`aec_on=0`，读取和 AFE
  帧数持续增长、`partial=0`，无启动错误；首遍人声和播报切换继续以现场结果验收。
- 9 月 15 日再次反馈无法稳定打断。日志确认播报时唤醒/AEC 开启、采样无丢帧，
  也捕获到一次检测后 197 ms 重新收音；另一次检测出现在播报结束附近。
  40 秒实录的 640,000 个麦克风采样点中，576 个达到绝对值 32,760 及以上。
  将末尾 16 秒的同一份麦克风/参考输入实时回放：VOIP、SR、FD AEC 均检出一次，
  检测点分别为 15,744 / 15,552 / 15,744 ms。SR 在该样本提前 192 ms，
  没有证明检测成功率提高，也不能把安静时命中视为播报打断成功。
  据此将唤醒 AEC 改为 SR，并把播放音量从 −24 调至 −30 dB；实际打断效果须复测。
  模式用途参考 [ESP-SR AEC 文档](https://docs.espressif.com/projects/esp-sr/en/latest/esp32s3/acoustic_echo_cancellation/README.html)。
- SR AEC 修正版占 1,957,283 字节（62%）；编译、关闭 ESP-SR 的语法检查、COM39
  应用和模型烧录校验通过。90 秒联网检查与串口注入的播报打断测试通过，内部堆最低
  约 88 KB，采集/编码/解码/输出错误为 0。服务器结束会话后正常回到 Idle。
  该结果仅验证软件控制和运行稳定性，真人连续播报打断仍待复测确认。
- 随后用户确认 SR AEC 修正版的真人播报打断正常；实录捕获到播报中的真实命中，
  检测后 195 ms 重新收音。按用户后续要求，示例播报音量再调至 100%（0 dB）。
  已确认的声学打断结果对应此前 −30 dB 版本，最大音量下的声学表现需另行观察。
- 最大音量版本占 1,957,319 字节，COM39 烧录校验及启动/联网/播放检查通过，
  串口确认 `output=0.0 dB`，音频输出错误为 0。播报时麦克风峰值再次达到 32,768、
  RMS 最高观测到 28,338，已出现输入削顶；本次按用户要求保留最大音量，不能沿用
  −30 dB 版本的真人打断验收结论。
- 输出 0 dB、播报麦克风增益 0 dB 版本（1,957,967 字节）已烧录到 COM39。
  增益切换、联网和串口注入打断通过，持续满幅削顶明显减少；用户现场确认真实
  唤醒词仍不能打断。随后按用户允许降低音量的要求，将输出调到 −12 dB、
  播报麦克风增益调到 12 dB。
- −12 dB 输出、12 dB 播报输入版本（1,957,967 字节）已编译并烧录到 COM39，
  固件和模型校验、启动/联网/播放检查通过。播报时输入为 12 dB，静音后恢复
  24 dB，本次测试的 `clip=0`，采集/编码/解码/输出错误均为 0。
  “小红帽”故事播报中捕获到真实“你好小智”命中，31 ms 分派、123 ms 重新收音，
  随后回复“你好呀”。完整 PCM 已保存到本地 `20260915-124222` 诊断目录。
  用户现场确认“能打断，音量够用”，当前示例保留此配置；以上延迟从检测命中计起。

原始串口日志保存在本地 `.build/ojoy-logs/serial-*.log`，不提交到仓库。
日志目录位于固件构建目录之外，避免 Arduino 清理构建缓存时丢失诊断数据。
