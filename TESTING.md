# 测试说明

测试分为三层，三者不能互相替代：

- `tests/run_checks.py` 是主机端快速检查，验证库结构、禁止依赖、元数据、协议常量和独立 golden vector。它不会执行 C++ 生产代码。
- `extras/tests/arduino/CompileSmoke` 和 `ProtocolSelfTest` 会调用真实库代码，覆盖会话、状态、MCP、Provisioning 和 WebSocket 帧；编译矩阵只证明它们能编译、链接，不代表断言已经运行。
- 板上运行测试需要上传 self-test 并检查串口输出中的 `PASS`。音频、电源、显示、网络稳定性和 OTA 仍需对应硬件测试。

## 主机与编译检查

```powershell
python .\tests\run_checks.py

powershell -ExecutionPolicy Bypass -File .\tests\compile_matrix.ps1 `
  -ArduinoWebsocketsPath <ArduinoWebsockets-library-path>
```

`compile_matrix.ps1` 编译 ESP32、S3、C3、C5、C6、P4，并额外编译 `ProtocolSelfTest`。若不传 `ArduinoWebsocketsPath`，脚本会明确报告真实 WebSocket 适配器分支被跳过；发布验证不得跳过。

显示库保持独立，仅在显式提供依赖路径时编译：

```powershell
powershell -ExecutionPolicy Bypass -File .\tests\compile_displays.ps1 `
  -ArduinoWebsocketsPath <path> -U8g2Path <path> `
  -TftEsPiPath <path> -LvglPath <path>
```

## 板上执行协议测试

先编译并上传 `extras/tests/arduino/ProtocolSelfTest`，再打开 115200 波特率串口：

```powershell
arduino-cli compile --fqbn esp32:esp32:esp32s3 `
  --library <xiaozhi-arduino-path> `
  .\extras\tests\arduino\ProtocolSelfTest
arduino-cli upload -p <COM-port> --fqbn esp32:esp32:esp32s3 `
  .\extras\tests\arduino\ProtocolSelfTest
arduino-cli monitor -p <COM-port> --config baudrate=115200
```

成功标志是 `Xiaozhi protocol self-test: PASS (0 failures)`。没有连接实体板时，应将结果写成“板上测试未执行”，不能把编译成功等同于运行通过。

## ESP32-S3 Wi-Fi/WebSocket 板上烟测

`extras/tests/arduino/HardwareWebSocketSmoke` 覆盖 Wi-Fi、ESP32 身份信息、真实
ArduinoWebsockets 适配器、协议 v3 文本与二进制双向收发、MCP 请求/响应、
服务端事件、状态变化、采集回调和下行音频回调。它读取串口依次输入的 SSID、
密码和测试 URL，不把凭据写入 sketch 或构建产物；调用
`WiFi.persistent(false)` 避免把本次 Wi-Fi 配置写入 NVS。

先在与 ESP32 同一局域网的电脑上启动只依赖 Python 标准库的测试服务端：

```powershell
python .\tests\hardware_ws_server.py --host 0.0.0.0 --port 8765
```

随后安装 ArduinoWebsockets，编译、上传该 sketch，并以 115200 波特率按提示发送
三行输入；第三行形如 `ws://<电脑局域网 IPv4>:8765/xiaozhi`。建议在构建真实
WebSocket 分支时将 `_WS_CONFIG_MAX_MESSAGE_SIZE=16384` 同时传给依赖库。

板端必须打印 `HARDWARE_SMOKE_PASS`，服务端必须打印 `SERVER_PASS`，二者同时
出现才算通过。测试中的 Opus 内容是用于验证封包与回调路由的占位字节，不代表
麦克风、扬声器、Codec 或音质已经通过测试。

## 尚需实体硬件验证

- 麦克风、扬声器、I2S、外部 Codec 和 PCM↔Opus 扩展。
- Wi-Fi 断线重连、TLS 证书轮换和生产服务端互通。
- U8g2/TFT_eSPI/LVGL 的引脚、颜色、刷新和中文字库。
- PSRAM/无 PSRAM 板、P4 外部网络、真实固件 OTA。
- 8–24 小时连续会话和网络抖动；记录最小堆、队列水位和丢帧。
