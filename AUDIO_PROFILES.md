# 音频硬件 Profile 配置指南

`TftEsPiDisplay` 使用编译期音频 Profile 选择麦克风、扬声器和 Codec 后端。它的用法类似 U8g2 的构造器：先在 `examples/TftEsPiDisplay/BoardConfig.h` 选择一个 Profile，再填写该方案需要的引脚和电气参数。没有选中的 ES8311、PDM 和 WakeNet 代码会被预处理器排除。

本文只描述当前代码已经实现的能力。官方 `xiaozhi-esp32-2.4.0` 中的 ES7210/TDM 麦克风阵列和 ES8388 等方案尚未成为本示例的内置后端，边界见“尚未内置的官方方案”。

## 快速开始

1. 打开 `examples/TftEsPiDisplay/BoardConfig.h`。
2. 将 `XIAOZHI_AUDIO_PROFILE` 改为下表中的一个值。
3. 填写该 Profile 使用的 `BOARD_AUDIO_*` 引脚、采样率和增益。
4. 根据是否需要本地唤醒设置 `XIAOZHI_AUDIO_ENABLE_WAKE_ESP_SR` 为数值 `0` 或 `1`。
5. 首次烧录时按文末清单逐项验收；编译成功不等于音频硬件已经工作。

未定义 `XIAOZHI_AUDIO_PROFILE` 时，`I2sOpusAudioPort.h` 为兼容旧代码会选择 ES8311；示例的 `BoardConfig.h` 也显式选择 ES8311。移植新板时建议始终显式定义，不依赖默认值。

## Profile 与器件别名

| Profile/别名 | 输入 | 输出 | 总线方式 | 内置 Codec 控制 | 默认输入/输出采样率 |
| --- | --- | --- | --- | --- | --- |
| `XIAOZHI_AUDIO_PROFILE_ES8311` | ES8311 ADC，标准 I2S | ES8311 DAC，标准 I2S | 共享 I2S | ES8311 | 24/24 kHz |
| `XIAOZHI_AUDIO_PROFILE_I2S_DUPLEX` | 数字 I2S 麦克风 | I2S DAC/功放 | 共享 BCLK/WS 和控制器 | 无 | 24/24 kHz |
| `XIAOZHI_AUDIO_PROFILE_I2S_SIMPLEX` | 数字 I2S 麦克风 | I2S DAC/功放 | 两个独立 I2S 控制器 | 无 | 16/24 kHz |
| `XIAOZHI_AUDIO_PROFILE_PDM_I2S` | PDM 麦克风 | 标准 I2S DAC/功放 | 两个独立 I2S 控制器 | 无 | 16/24 kHz |
| `XIAOZHI_AUDIO_PROFILE_CUSTOM_CODEC` | 标准 I2S | 标准 I2S | 默认共享 | 三个用户回调 | 24/24 kHz |
| `XIAOZHI_AUDIO_PROFILE_INMP441_MAX98357A` | INMP441 类标准 I2S Mic | MAX98357A | `I2S_SIMPLEX` 的别名 | 无 | 16/24 kHz |
| `XIAOZHI_AUDIO_PROFILE_MSM261_MAX98357A` | MSM261 类标准 I2S Mic | MAX98357A | `I2S_SIMPLEX` 的别名 | 无 | 16/24 kHz |
| `XIAOZHI_AUDIO_PROFILE_MP34DT05_MAX98357A` | MP34DT05 类 PDM Mic | MAX98357A | `PDM_I2S` 的别名 | 无 | 16/24 kHz |

器件别名只选择底层后端和通用格式默认值，不会自动填写板卡引脚、供电使能或模块特有的时钟极性。别名的 `compiledProfileName()` 返回底层通用 Profile 名称，这是正常现象。

## 依赖和固件裁剪

Profile 会在 `I2sOpusAudioPort.h` 中派生数值为 `0` 或 `1` 的 `XIAOZHI_AUDIO_ENABLE_*` 宏，并通过 `#if` 裁剪实现和头文件：

- 所有 Profile 都使用 `<EspressifOpus.h>`，因为上下行音频都需要 Opus。
- 只有 ES8311 Profile 会编译 `<Wire.h>`、`<EspressifEs8311.h>` 和 `es8311_reg.h`。
- 只有 PDM Profile 会编译 `<driver/i2s_pdm.h>`；标准 I2S Profile 不引入 PDM 后端。
- 只有 `XIAOZHI_AUDIO_ENABLE_WAKE_ESP_SR == 1` 时才编译 ESP-SR/WakeNet 头文件、模型加载和唤醒任务。
- Custom Profile 不会引入 ES8311 驱动。自定义 Codec 所需的库由应用按同样的 Profile 条件自行包含。

底层宏包括：

- `XIAOZHI_AUDIO_ENABLE_INPUT_I2S_STD`
- `XIAOZHI_AUDIO_ENABLE_INPUT_I2S_PDM`
- `XIAOZHI_AUDIO_ENABLE_OUTPUT_I2S_STD`
- `XIAOZHI_AUDIO_ENABLE_I2S_SHARED`
- `XIAOZHI_AUDIO_ENABLE_I2S_SEPARATE`
- `XIAOZHI_AUDIO_ENABLE_CODEC_ES8311`
- `XIAOZHI_AUDIO_ENABLE_AMP_GPIO`
- `XIAOZHI_AUDIO_ENABLE_WAKE_ESP_SR`

普通板卡只应选择 `XIAOZHI_AUDIO_PROFILE`，不要逐个打开底层宏。高级 Custom 配置可以在包含 `I2sOpusAudioPort.h` 之前覆盖它们，但值必须是数值 `0` 或 `1`；多编译后端会增加固件和依赖，也更容易构造出运行时配置与编译后端不一致的组合。

`library.properties` 只声明 Xiaozhi 核心依赖。使用本完整示例时，仍需根据选项安装对应的 Opus、ES8311 或 ESP-SR 组件。

## BoardConfig 公共字段

`BoardAudioConfig.h::makeConfig()` 会将现有 `BOARD_AUDIO_*` 宏映射到 `I2sOpusAudioPort::Config`：

| BoardConfig 宏 | 作用 |
| --- | --- |
| `BOARD_AUDIO_OUTPUT_I2S_PORT` | 扬声器 I2S 控制器编号 |
| `BOARD_AUDIO_OUTPUT_SAMPLE_RATE` | 扬声器硬件采样率 |
| `BOARD_AUDIO_OUTPUT_MCLK/BCLK/WS/DATA` | 从 ESP32 视角定义的 TX 引脚；`DATA` 接 Codec/DAC/功放的 DIN |
| `BOARD_AUDIO_OUTPUT_MCLK_INVERTED/BCLK_INVERTED/WS_INVERTED` | 输出时钟极性 |
| `BOARD_AUDIO_OUTPUT_DATA_BITS/VALID_BITS/SLOT_BITS` | 输出 DMA 位宽、有效位数和总线 slot 位宽 |
| `BOARD_AUDIO_OUTPUT_CHANNELS/SLOT` | 输出 DMA 声道数及 `Left`/`Right`/`Both` slot |
| `BOARD_AUDIO_INPUT_I2S_PORT` | 标准 I2S 麦克风控制器编号 |
| `BOARD_AUDIO_INPUT_SAMPLE_RATE` | 标准 I2S 麦克风硬件采样率 |
| `BOARD_AUDIO_INPUT_MCLK/BCLK/WS/DATA` | 标准 I2S RX 引脚；`DATA` 接麦克风 SD/DOUT |
| `BOARD_AUDIO_INPUT_MCLK_INVERTED/BCLK_INVERTED/WS_INVERTED` | 输入时钟极性 |
| `BOARD_AUDIO_INPUT_DATA_BITS/VALID_BITS/SLOT_BITS` | 输入 DMA 位宽、有效位数和总线 slot 位宽 |
| `BOARD_AUDIO_INPUT_CHANNELS/SLOT/RIGHT_SHIFT` | 输入声道、slot 和 32→16 位转换右移量 |
| `BOARD_AUDIO_CAPTURE_CHANNEL` | 双声道输入最终送入 Opus 的 `Left`、`Right` 或 `Auto` |
| `BOARD_AUDIO_PDM_I2S_PORT/SAMPLE_RATE/CLOCK/DATA` | PDM 麦克风端点 |
| `BOARD_AUDIO_PDM_CLOCK_INVERTED` | PDM 时钟极性 |
| `BOARD_AUDIO_AMP_ENABLE_PIN/ACTIVE_LEVEL` | 无 Codec 方案的 MAX98357A SD/EN 等使能控制；固定有效时用 `-1` |
| `BOARD_AUDIO_AMP_VOLUME_PERCENT` | 无 Codec 32-bit 输出的软件音量，范围 `0..100` |
| `BOARD_AUDIO_MCLK_MULTIPLE` | 标准 I2S 的 MCLK 倍频，默认 256 |
| `BOARD_AUDIO_I2C_SDA/SCL/FREQUENCY` | 内置 ES8311 控制总线 |
| `BOARD_AUDIO_CODEC_ADDRESS` | ES8311 地址，默认 `0x18` |
| `BOARD_AUDIO_CODEC_INITIALIZE_WIRE` | 是否由音频端口初始化 I2C 总线 |
| `BOARD_AUDIO_CODEC_RESET_ON_BEGIN/RESET_DELAY_MS` | ES8311 启动复位及等待时间 |
| `BOARD_AUDIO_CODEC_PA_PIN/PA_ACTIVE_LEVEL` | ES8311 路径控制的外部 PA GPIO 和有效电平；不用时 pin 为 `-1` |
| `BOARD_AUDIO_CODEC_USE_MCLK/MASTER/NO_DAC_REFERENCE` | ES8311 时钟角色和 DAC reference 行为 |
| `BOARD_AUDIO_PA_SUPPLY_VOLTAGE` | ES8311 驱动的 PA 供电参数 |
| `BOARD_AUDIO_CODEC_DAC_VOLTAGE` | ES8311 DAC 满量程电压参数 |
| `BOARD_AUDIO_PA_GAIN_DB` | ES8311 PA 增益参数 |
| `BOARD_AUDIO_MIC_GAIN_DB` | ES8311 麦克风增益 |
| `BOARD_AUDIO_OUTPUT_VOLUME_DB` | ES8311 输出音量 |

上表覆盖常用板级硬件初始化项。队列、任务栈、Opus 参数、语音调理和 Custom Codec 回调仍是运行时 `Config` 字段；需要时可在 `makeConfig()` 返回后调整。

## 配置 ES8311

在 `BoardConfig.h` 中选择：

```cpp
#define XIAOZHI_AUDIO_PROFILE XIAOZHI_AUDIO_PROFILE_ES8311
#define XIAOZHI_AUDIO_ENABLE_WAKE_ESP_SR 1
```

随后填写：

```cpp
#define BOARD_AUDIO_I2C_SDA 41
#define BOARD_AUDIO_I2C_SCL 42
#define BOARD_AUDIO_I2C_FREQUENCY 400000
#define BOARD_AUDIO_CODEC_ADDRESS 0x18

#define BOARD_AUDIO_OUTPUT_I2S_PORT 0
#define BOARD_AUDIO_OUTPUT_SAMPLE_RATE 24000
#define BOARD_AUDIO_OUTPUT_MCLK 46
#define BOARD_AUDIO_OUTPUT_BCLK 39
#define BOARD_AUDIO_OUTPUT_WS 2
#define BOARD_AUDIO_OUTPUT_DATA 38  // ESP32 -> ES8311

#define BOARD_AUDIO_INPUT_I2S_PORT 0
#define BOARD_AUDIO_INPUT_SAMPLE_RATE 24000
#define BOARD_AUDIO_INPUT_MCLK 46
#define BOARD_AUDIO_INPUT_BCLK 39
#define BOARD_AUDIO_INPUT_WS 2
#define BOARD_AUDIO_INPUT_DATA 40   // ES8311 -> ESP32
```

ES8311 内置后端要求：

- RX/TX 使用 `BusMode::Shared`。
- 输入、输出采样率相同。
- 两端都是 16-bit DMA、16-bit slot、双声道 `Both` 格式。
- 输入输出的 port、MCLK、BCLK、WS 及其反相设置完全相同，只有数据引脚不同。
- `useMclk == true` 时 MCLK 引脚不能为 `-1`。
- 当前 ESP32 I2S channel 固定为时钟主机，因此 `BOARD_AUDIO_CODEC_MASTER` 必须为 `false`；只把 Codec 改成主机将造成双主冲突并被初始化校验拒绝。
- `wire` 不能为空；`initializeWire == true` 时 SDA/SCL 必须有效。
- 由音频端口初始化 I2C 时，频率必须大于 0，SDA/SCL 必须互异，且不能与 I2S 或 PA GPIO 重叠。

`BoardAudioConfig.h` 已将 `wire` 设为全局 `Wire`，并映射 I2C、地址、模拟增益、复位、PA 与 MCLK 宏。例如：

```cpp
#define BOARD_AUDIO_CODEC_PA_PIN 13
#define BOARD_AUDIO_CODEC_PA_ACTIVE_LEVEL true
#define BOARD_AUDIO_CODEC_USE_MCLK true
#define BOARD_AUDIO_CODEC_RESET_ON_BEGIN true
#define BOARD_AUDIO_CODEC_RESET_DELAY_MS 10
```

ES8311 路径会在静音时把 `BOARD_AUDIO_CODEC_PA_PIN` 拉到非有效电平，播放前在 Codec 解除静音成功后再使能 PA，以减少空闲底噪和启停爆音。该 GPIO 不能与 I2S/I2C 端点复用。`BOARD_AUDIO_AMP_ENABLE_PIN` 属于无 Codec 的 `AmplifierControl`，不会替代 `hardware.es8311.paPin`。

## 配置共享 I2S 麦克风和扬声器

适用于数字麦克风与 I2S DAC/功放能够共享 BCLK/WS、同一采样率和同一数据格式的硬件：

```cpp
#define XIAOZHI_AUDIO_PROFILE XIAOZHI_AUDIO_PROFILE_I2S_DUPLEX
```

在 `BoardConfig.h` 中令输入输出使用相同的：

- `*_I2S_PORT`
- `*_SAMPLE_RATE`
- `*_MCLK/BCLK/WS`
- 三个 `*_INVERTED` 值

`BOARD_AUDIO_INPUT_DATA` 和 `BOARD_AUDIO_OUTPUT_DATA` 分别连接麦克风输出与功放输入，可以不同。没有 MCLK 的模块可将输入输出 MCLK 都设为 `-1`。

共享后端要求输入输出的控制器、采样率、MCLK/BCLK/WS、极性、`dataBits`、`slotBits` 和 channel mode 相同，确保 ESP-IDF 全双工硬件格式一致；RX/TX 会分别初始化 slot mask，因此允许单声道扬声器使用 `Both` 而麦克风使用 `Right` 等常见 Codec 布局。默认值为 32-bit DMA、32-bit slot、单声道左 slot；输入有效位为 24、`rightShift=12`，输出有效位为 16。若两端需要不同位宽、channel mode 或时钟，应改用独立 I2S，而不是绕过校验。

## 配置独立 I2S 麦克风和扬声器

INMP441/MSM261 + MAX98357A 可选择通用 Profile 或器件别名：

```cpp
#define XIAOZHI_AUDIO_PROFILE \
  XIAOZHI_AUDIO_PROFILE_INMP441_MAX98357A
// 或 XIAOZHI_AUDIO_PROFILE_MSM261_MAX98357A
```

填写两套独立端点：

```cpp
#define BOARD_AUDIO_OUTPUT_I2S_PORT 0
#define BOARD_AUDIO_OUTPUT_SAMPLE_RATE 24000
#define BOARD_AUDIO_OUTPUT_MCLK -1
#define BOARD_AUDIO_OUTPUT_BCLK 5
#define BOARD_AUDIO_OUTPUT_WS 6
#define BOARD_AUDIO_OUTPUT_DATA 4

#define BOARD_AUDIO_INPUT_I2S_PORT 1
#define BOARD_AUDIO_INPUT_SAMPLE_RATE 16000
#define BOARD_AUDIO_INPUT_MCLK -1
#define BOARD_AUDIO_INPUT_BCLK 10
#define BOARD_AUDIO_INPUT_WS 9
#define BOARD_AUDIO_INPUT_DATA 18
```

独立模式允许输入、输出使用不同采样率、格式、slot 和时钟极性，但两个 I2S port 必须不同。默认标准 I2S 输入采用 24-in-32、左 slot、`rightShift=12`；MAX98357A 输出采用 16 个有效位装入 32-bit slot。标准模式固定使用 Philips I2S 格式，当前没有 Left-Justified/DSP 格式选择项。

选择独立 Profile 后，示例会把第二个 I2S 主控制器的输入 MCLK/BCLK/WS/DATA 设为 `-1`，强制移植者填写真实独立引脚。输入输出端点不能复用任何有效 GPIO，防止两个不同采样率的主控制器同时驱动同一时钟线。

若 MAX98357A 的 SD/EN 脚由 MCU 控制：

```cpp
#define BOARD_AUDIO_AMP_ENABLE_PIN 7
#define BOARD_AUDIO_AMP_ACTIVE_LEVEL true
#define BOARD_AUDIO_AMP_VOLUME_PERCENT 70
```

SD/EN 已在硬件上拉并始终有效时保持 `BOARD_AUDIO_AMP_ENABLE_PIN -1`。
使能 pin 不能小于 `-1`，也不能与任一麦克风、PDM 或扬声器端点 GPIO 复用。

## 配置 PDM 麦克风

MP34DT05 类 PDM 麦克风配 MAX98357A：

```cpp
#define XIAOZHI_AUDIO_PROFILE \
  XIAOZHI_AUDIO_PROFILE_MP34DT05_MAX98357A
```

填写 PDM 输入，而不是标准 I2S 输入：

```cpp
#define BOARD_AUDIO_PDM_I2S_PORT 0
#define BOARD_AUDIO_PDM_SAMPLE_RATE 16000
#define BOARD_AUDIO_PDM_CLOCK 45
#define BOARD_AUDIO_PDM_DATA 46
#define BOARD_AUDIO_PDM_CLOCK_INVERTED false

#define BOARD_AUDIO_OUTPUT_I2S_PORT 1
#define BOARD_AUDIO_OUTPUT_SAMPLE_RATE 24000
#define BOARD_AUDIO_OUTPUT_MCLK -1
#define BOARD_AUDIO_OUTPUT_BCLK 18
#define BOARD_AUDIO_OUTPUT_WS 16
#define BOARD_AUDIO_OUTPUT_DATA 17
```

PDM 后端由 ESP32 I2S 硬件解调为 16-bit mono PCM。PDM 输入 port 与标准 I2S 输出 port 必须不同，PDM CLK/DATA 也不能与任何扬声器端点 GPIO 重叠。标准输入的 `BOARD_AUDIO_INPUT_*` 宏在此 Profile 下不会映射到配置，可以保留占位值，但不会编译标准 I2S RX 后端。

## 配置 Custom Codec

Custom Profile 复用当前标准 I2S 管线，只把 Codec 的初始化、静音和释放交给应用。ESP32 仍固定为 I2S 时钟主机，自定义回调不能把 Codec 也配置成主机：

```cpp
#define XIAOZHI_AUDIO_PROFILE XIAOZHI_AUDIO_PROFILE_CUSTOM_CODEC
```

其默认硬件格式是共享 I2S、24 kHz、16-bit DMA、16-bit slot、双声道 `Both`。在 `BoardConfig.h` 中像 ES8311 一样填写共享 I2S 引脚，但自定义 Codec 的 I2C/SPI 控制由回调负责。

三个回调必须全部提供：

```cpp
struct MyCodecContext {
  // 保存用户驱动句柄；对象必须比 I2sOpusAudioPort 活得更久。
};

static MyCodecContext myCodec;

static bool beginMyCodec(
    void* opaque, const I2sOpusAudioPort::CodecFormat& format) {
  auto* context = static_cast<MyCodecContext*>(opaque);
  (void)context;
  // 使用 format.inputSampleRate、outputSampleRate、bitsPerSample、
  // inputChannels、outputChannels 和 mclkMultiple 初始化硬件。
  return true;
}

static bool muteMyCodec(void* opaque, bool muted) {
  auto* context = static_cast<MyCodecContext*>(opaque);
  (void)context;
  (void)muted;
  return true;
}

static void endMyCodec(void* opaque) {
  auto* context = static_cast<MyCodecContext*>(opaque);
  (void)context;
}

static I2sOpusAudioPort::Config makeAudioConfig() {
  auto config = xiaozhi_audio_board::makeConfig();
  config.codec.context = &myCodec;
  config.codec.begin = beginMyCodec;
  config.codec.setMuted = muteMyCodec;
  config.codec.end = endMyCodec;
  return config;
}

const I2sOpusAudioPort::Config audioConfig = makeAudioConfig();
```

只提供部分回调会被拒绝。Custom Codec 的 PA/静音也应由 `setMuted` 管理；非 ES8311 Profile 的 `probe()` 只返回 `true`，真正的控制总线探测和初始化成功与否由 `begin` 回调报告。

Custom Profile 默认只编译共享标准 I2S。若自定义器件需要独立 I2S，必须在包含音频头之前同时覆盖底层 shared/separate 宏，并把 `hardware.busMode` 和两个 port 改成匹配值。不要用 Custom 回调假装支持当前管线没有实现的 TDM RX。

## 24-in-32、rightShift 和 slot

标准 I2S 端点由 `StandardI2sEndpoint` 描述：

- `dataBits`：DMA 中每个样本的 C++ 存储宽度，当前只支持 16 或 32。
- `validBits`：输入端记录麦克风声明的有效位数；输出源固定为 PCM16，因此输出端必须设为 16。
- `slotBits`：I2S slot 宽度，支持 16、24、32，且不能小于 `dataBits`。
- `rightShift`：仅用于 32-bit 麦克风输入。读取到有符号 `int32_t` 后先右移，再饱和转换成 PCM16。
- `channels`：DMA 帧中的声道数，支持 1 或 2。
- `slot`：I2S 外设使用 `Left`、`Right` 或 `Both` slot。
- `captureChannel`：当 DMA 确实含两个声道时，决定最终送往 Opus 的 `Left`、`Right` 或 `Auto` 单声道；它与外设的 `slot` 不是同一个概念。

INMP441/MSM261 别名当前采用与官方直连 I2S 路径一致的通用默认值：

```cpp
input.dataBits = 32;
input.validBits = 24;
input.slotBits = 32;
input.channels = 1;
input.slot = I2sOpusAudioPort::I2sSlot::Left;
input.rightShift = 12;
```

`validBits=24` 不会自动计算或替代 `rightShift`；采集转换实际使用 `rightShift`。不同模块、批次和 Arduino I2S 驱动对有效位的对齐可能不同。默认 12 是兼容官方实现的起点：波形削顶时增大该值，电平过低时减小，并同时观察原始 RMS、峰值和噪声底，不能只凭“24 位”猜测。

这些常用格式可以直接在 `BoardConfig.h` 调整：

```cpp
#define BOARD_AUDIO_INPUT_DATA_BITS 32
#define BOARD_AUDIO_INPUT_VALID_BITS 24
#define BOARD_AUDIO_INPUT_SLOT_BITS 32
#define BOARD_AUDIO_INPUT_CHANNELS 1
#define BOARD_AUDIO_INPUT_SLOT I2sOpusAudioPort::I2sSlot::Right
#define BOARD_AUDIO_INPUT_RIGHT_SHIFT 12
```

如果配置 `slot=Both` 和 `channels=2`，还应根据接线设置：

```cpp
#define BOARD_AUDIO_CAPTURE_CHANNEL \
  I2sOpusAudioPort::CaptureChannel::Right
// 或 CaptureChannel::Auto，让运行时按左右声道能量选择。
```

`BoardConfig.h` 默认把 `captureChannel` 设为 `Left`。共享 I2S Profile 只允许 RX/TX 的 slot mask、输入有效位和输入右移量不同；数据/slot 位宽、channel mode、采样率和公共时钟必须一致。独立 Profile 才能分别设置位宽、channel mode 和采样率。

输出为 32-bit DMA 时，管线会把 PCM16 固定左移 16 位，放入 32-bit 字的最高有效位；无 Codec 方案的软件音量也在这一输出路径中生效。

## WakeNet 编译开关

`XIAOZHI_AUDIO_ENABLE_WAKE_ESP_SR` 是编译开关，不是普通运行时选项：

```cpp
// 编译 WakeNet、ESP-SR 依赖、模型加载和 wake task。
#define XIAOZHI_AUDIO_ENABLE_WAKE_ESP_SR 1

// 完全移除以上代码和依赖。
#define XIAOZHI_AUDIO_ENABLE_WAKE_ESP_SR 0
```

`Config::forCompiledProfile()` 会令 `enableWakeDetection` 与该宏保持一致。宏为 0 后再手动把 `enableWakeDetection` 改成 `true`，初始化会明确失败，而不会静默忽略。

宏为 1 时还必须满足：

- 工程能提供 `<ESP_SR.h>` 和 WakeNet 相关头文件。
- 分区表中存在 `wakeModelPartition` 指定的模型分区，默认名为 `model`。
- 分区中包含可用 WakeNet 模型；`wakeModelKeyword` 为 `nullptr` 时使用第一个 WakeNet 模型，否则按子串筛选。
- 模型采样率必须与送入唤醒器的单声道 capture format 匹配。
- 目标芯片和所用 Arduino-ESP32/ESP-SR 版本实际支持该模型。打开宏不能让不受支持的 SoC 获得 WakeNet 能力。

`enableWakeDetection=false` 只是在运行时不启动检测，不能减小已经编译的 ESP-SR 固件；需要减小固件时应把宏设为 0。

## SoC 和格式限制

- 标准 I2S 依赖 Arduino-ESP32 提供的新 I2S channel API 和 `<driver/i2s_std.h>`。
- 独立 I2S/PDM Profile 要求输入与输出使用不同 port；目标的 `SOC_I2S_NUM` 必须至少覆盖所填编号。只有一个 I2S 控制器的芯片不能使用默认独立方案。
- PDM Profile 在编译时检查 `SOC_I2S_SUPPORTS_PDM_RX`；不支持时直接报错。
- 所有硬件输入和输出采样率必须大于 0 且能被 100 整除，以保证支持的 10/20/40/60 ms 音频帧映射为整数样本数。
- 标准 I2S 的 BCLK、WS 和 DATA 必须为有效 GPIO；MCLK 一般可以为 `-1`，但 ES8311 的 `useMclk=true` 时不能省略。
- ES8311 和共享 I2S 必须同 port、同采样率、同公共时钟及格式。
- 独立模式默认使用 port 0 输出、port 1 输入；PDM 默认 port 0 输入、port 1 输出。实际端口可由 BoardConfig 修改，但必须存在且不同。
- 当前标准 I2S 后端使用 Philips 格式，只实现 16/32-bit DMA、1/2 声道及 Left/Right/Both slot。
- 单声道端点可选择 Left、Right 或由 I2S 硬件复制到 Both；双声道端点必须选择 Both。16-bit 输入必须声明 16 个有效位且不能再配置 `rightShift`；输出有效位固定为 16。
- Profile 只负责软件能力选择，不检查 GPIO 是否与 Flash/PSRAM、USB、启动绑带、显示或 SD 卡冲突。

## 尚未内置的官方方案

官方源码还包含以下常见硬件，但当前 `I2sOpusAudioPort` 没有对应内置 Profile：

- **ES8311 + ES7210 麦克风阵列**：官方 `BoxAudioCodec` 使用标准 I2S TX 和四 slot TDM RX，可提供麦克风及播放参考通道。当前 Arduino 音频端口没有 TDM RX 后端，Custom Codec 回调只能初始化控制芯片，不能补出缺失的 TDM 数据通路，因此不能仅靠回调完整支持 ES7210。
- **ES8388**：当前没有 ES8388 控制驱动或专用 Profile。它的普通标准 I2S 单麦/播放路径理论上可由 Custom Codec 回调接入，但 I2C 寄存器初始化、增益、静音、PA 和电源管理均由应用负责。
- **ES8388/ES7210 的硬件 reference 与设备端 AEC**：当前管线最终选择一个 capture channel 转为单声道，没有移植官方 ESP-SR AFE 的双通道 reference/AEC 数据流。配置两声道不等于已经获得设备端 AEC。
- **ES8374、ES8389、ES8156、ES7243E、AW88298**：均无内置控制驱动，只能在其 I2S 格式已受支持时通过 Custom Codec 自行接入。

因此，文档和代码不提供不存在的 `XIAOZHI_AUDIO_PROFILE_ES7210` 或 `XIAOZHI_AUDIO_PROFILE_ES8388` 宏。后续若增加 ES7210，应先实现并验证独立的 TDM RX 后端；若增加 ES8388，应同时实现控制驱动、参考通道语义和对应硬件测试，而不只是增加一个别名。

## 硬件验收清单

### 上电前

- 核对麦克风、Codec、PA 和扬声器的供电电压、公共地、扬声器阻抗与 PA 最大功率。
- 核对 ESP32 视角的 `DATA` 方向：输出 DATA 接外设 DIN，输入 DATA 接外设 DOUT/SD。
- 检查 GPIO 是否与 Flash/PSRAM、USB、启动绑带、LCD、触摸、SD 卡或其他外设冲突。
- 确认数字麦克风的 L/R 选择脚、MAX98357A SD/EN 脚及 PA 有效电平。
- ES8311 板确认 I2C 地址、MCLK 要求、PA 是否由 GPIO 控制及其有效电平。

### 首次启动

- 串口打印的 `compiledProfileName()` 与预期一致。
- `audioPort.begin()` 成功，没有“backend was not compiled”、无效端点、I2S port 不存在或共享格式不一致错误。
- ES8311 的 `probe()` 能读到配置地址；注意直接 I2S、PDM 和 Custom Profile 的 `probe()` 返回 `true` 并不代表硬件真实存在。
- 用示波器或逻辑分析仪确认 BCLK、WS、MCLK/PDM CLK 的频率、极性和上电/静音时序。

### 麦克风

- 安静环境下原始 RMS 稳定且不为 0，讲话时明显上升。
- 波形没有持续满幅、直流偏置或每隔一帧跳变；如有问题，优先核对 slot、channels、有效位对齐和 `rightShift`。
- 左/右 slot 与模块 L/R 焊盘一致；双声道模式分别验证 `CaptureChannel::Left/Right/Auto`。
- 录制并回放至少 30 秒语音，检查语速、音调、爆音、混叠和包边界杂音。

### 扬声器和静音

- 低音量开始播放，确认采样率和 slot 正确，没有音调加快/变慢或左右帧错位。
- 检查软件音量、Codec 音量以及 PA 增益没有重复放大导致削顶。
- 多次开始/停止播放和用户打断，确认 SD/EN、PA 或 Codec mute 极性正确且无明显爆音。
- 确认停止播放后没有旧 DMA 尾音，重新开始不会丢失首包。

### 唤醒与会话

- WakeNet 编译开启时，确认模型分区成功加载、模型名和采样率正确。
- 分别测试近讲、远讲、扬声器播放期间、安静和背景噪声下的唤醒率与误唤醒率。
- 测量唤醒到 Listening、说话结束到首个 TTS、打断到静音的 P50/P95 延迟。
- 连续问答、快速打断、断网重连和重新建会话时，不应出现上一代音频串入。

### 稳定性

- 观察 `[audio-perf]` 中编码、解码、WakeNet 最大耗时，队列深度、drop/reject/stale 计数、最低堆、最大连续堆块和各任务栈余量。
- 在正常及弱 Wi-Fi 下分别运行至少 30 分钟，再进行 8～24 小时长稳测试。
- 反复执行结束/重新开始、静音/取消静音和唤醒开关，确认没有任务卡死、内存持续下降或 I2S 无法重新启用。
