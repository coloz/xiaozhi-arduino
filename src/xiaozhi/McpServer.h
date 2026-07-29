#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace xiaozhi {

enum class McpPropertyType : uint8_t {
    Boolean,
    Integer,
    String,
};

using McpValue = std::variant<bool, int32_t, std::string>;

struct McpProperty {
    std::string name;
    McpPropertyType type = McpPropertyType::String;
    bool required = true;
    bool has_default = false;
    McpValue default_value = std::string();
    bool has_range = false;
    int32_t minimum = 0;
    int32_t maximum = 0;

    static McpProperty Boolean(const std::string& name);
    static McpProperty Boolean(const std::string& name, bool default_value);
    static McpProperty Integer(const std::string& name);
    static McpProperty Integer(const std::string& name, int32_t minimum, int32_t maximum);
    static McpProperty Integer(const std::string& name, int32_t default_value,
                               int32_t minimum, int32_t maximum);
    static McpProperty String(const std::string& name);
    static McpProperty String(const std::string& name, const std::string& default_value);
};

class McpArguments {
public:
    bool contains(const std::string& name) const;
    bool getBool(const std::string& name, bool& value) const;
    bool getInt(const std::string& name, int32_t& value) const;
    bool getString(const std::string& name, std::string& value) const;

private:
    friend class McpServer;
    std::vector<std::pair<std::string, McpValue>> values_;
};

class McpResult {
public:
    enum class Kind : uint8_t {
        Text,
        Boolean,
        Integer,
        Json,
    };

    static McpResult Text(const std::string& value);
    static McpResult Boolean(bool value);
    static McpResult Integer(int32_t value);
    static McpResult Json(const std::string& value);
    static McpResult Error(const std::string& message);

    Kind kind() const { return kind_; }
    bool isError() const { return is_error_; }
    const std::string& stringValue() const { return string_value_; }
    int32_t integerValue() const { return integer_value_; }
    bool booleanValue() const { return boolean_value_; }

private:
    Kind kind_ = Kind::Text;
    bool is_error_ = false;
    bool boolean_value_ = false;
    int32_t integer_value_ = 0;
    std::string string_value_;
};

using McpHandler = std::function<McpResult(const McpArguments& arguments)>;
using McpUserToolAuthorizer = std::function<bool(const std::string& tool_name)>;

struct McpTool {
    std::string name;
    std::string description;
    std::vector<McpProperty> properties;
    McpHandler handler;
    bool user_only = false;
};

class McpServer {
public:
    explicit McpServer(std::string server_name = "xiaozhi-arduino",
                       std::string server_version = "2.4.0");

    bool addTool(McpTool tool, std::string* error = nullptr);
    bool removeTool(const std::string& name);
    void clearTools();
    size_t toolCount() const { return tools_.size(); }

    void setServerInfo(const std::string& name, const std::string& version);
    void setMaxResponseBytes(size_t bytes);
    // user_only tools are denied unless this callback is installed and returns true.
    // A UI can use it to require a local confirmation before a dangerous operation.
    void setUserToolAuthorizer(McpUserToolAuthorizer authorizer);

    bool handle(const uint8_t* data, size_t size, std::string& response,
                std::string& error) const;
    bool handle(const std::string& message, std::string& response, std::string& error) const {
        return handle(reinterpret_cast<const uint8_t*>(message.data()), message.size(), response,
                      error);
    }

private:
    std::vector<McpTool> tools_;
    std::string server_name_;
    std::string server_version_;
    size_t max_response_bytes_ = 8000;
    McpUserToolAuthorizer user_tool_authorizer_;
};

}  // namespace xiaozhi
