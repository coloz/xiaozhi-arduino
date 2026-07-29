/*
 * McpTool 示例
 *
 * 演示如何定义带参数约束和处理函数的 MCP 工具，并将其注册到 Xiaozhi 客户端。
 * 此草图使用不会联网的占位 Transport，只用于说明工具声明与调用参数读取方式；
 * 接入真实服务时请替换为可用的传输实现，并在 handler 中控制实际设备。
 *
 * Demonstrates how to define an MCP tool with parameter constraints and a handler,
 * then register it with the Xiaozhi client. The placeholder Transport does not
 * connect and exists only to illustrate tool declarations and argument handling;
 * replace it with a real transport and control the actual device in the handler.
 */

#include <Xiaozhi.h>

class ExampleTransport final : public xiaozhi::Transport {
 public:
  void setCallbacks(xiaozhi::TransportCallbacks callbacks) override {
    callbacks_ = std::move(callbacks);
  }
  bool connect(const xiaozhi::TransportRequest&) override { return false; }
  void loop() override {}
  bool sendText(const uint8_t*, size_t) override { return false; }
  bool sendBinary(const uint8_t*, size_t) override { return false; }
  void close() override {}
  bool connected() const override { return false; }

 private:
  xiaozhi::TransportCallbacks callbacks_;
};

ExampleTransport transport;
xiaozhi::Client client(transport);

void setup() {
  Serial.begin(115200);

  xiaozhi::McpTool tool;
  tool.name = "device.set_volume";
  tool.description = "Set speaker volume";
  tool.properties = {xiaozhi::McpProperty::Integer("volume", 50, 0, 100)};
  tool.handler = [](const xiaozhi::McpArguments& arguments) {
    int32_t volume = 0;
    arguments.getInt("volume", volume);
    Serial.printf("volume=%ld\n", static_cast<long>(volume));
    return xiaozhi::McpResult::Boolean(true);
  };

  std::string error;
  if (!client.mcp().addTool(std::move(tool), &error)) {
    Serial.println(error.c_str());
  }
}

void loop() {}
