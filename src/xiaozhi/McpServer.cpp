#include "McpServer.h"

#include <ArduinoJson.h>

#include <algorithm>
#include <cstring>
#include <limits>

namespace xiaozhi {
namespace {

constexpr int32_t kParseError = -32700;
constexpr int32_t kInvalidRequest = -32600;
constexpr int32_t kMethodNotFound = -32601;
constexpr int32_t kInvalidParams = -32602;
constexpr int32_t kInternalError = -32603;
constexpr int32_t kUserAuthorizationRequired = -32001;

struct RpcId {
    enum class Kind : uint8_t { Null, Signed, Unsigned, String };
    Kind kind = Kind::Null;
    int64_t signed_value = 0;
    uint64_t unsigned_value = 0;
    std::string string_value;
};

McpProperty makeProperty(const std::string& name, McpPropertyType type) {
    McpProperty property;
    property.name = name;
    property.type = type;
    return property;
}

const McpValue* findValue(
    const std::vector<std::pair<std::string, McpValue>>& values,
    const std::string& name) {
    for (const auto& item : values) {
        if (item.first == name) {
            return &item.second;
        }
    }
    return nullptr;
}

bool validName(const std::string& value, size_t maximum) {
    if (value.empty() || value.size() > maximum) {
        return false;
    }
    for (char character : value) {
        const bool allowed = (character >= 'a' && character <= 'z') ||
                             (character >= 'A' && character <= 'Z') ||
                             (character >= '0' && character <= '9') || character == '_' ||
                             character == '-' || character == '.';
        if (!allowed) {
            return false;
        }
    }
    return true;
}

bool copyJsonString(JsonVariantConst value, std::string& output,
                    size_t maximum = std::numeric_limits<size_t>::max()) {
    if (!value.is<const char*>()) {
        return false;
    }
    const JsonString string = value.as<JsonString>();
    if (string.size() > maximum) {
        return false;
    }
    output.assign(string.c_str(), string.size());
    return true;
}

void setError(std::string* destination, const std::string& value) {
    if (destination != nullptr) {
        *destination = value;
    }
}

bool parseRpcId(JsonVariantConst value, RpcId& output) {
    output = {};
    if (value.isNull()) {
        output.kind = RpcId::Kind::Null;
        return true;
    }
    std::string string;
    if (copyJsonString(value, string, 128)) {
        output.kind = RpcId::Kind::String;
        output.string_value = std::move(string);
        return true;
    }
    if (value.is<int64_t>()) {
        output.kind = RpcId::Kind::Signed;
        output.signed_value = value.as<int64_t>();
        return true;
    }
    if (value.is<uint64_t>()) {
        output.kind = RpcId::Kind::Unsigned;
        output.unsigned_value = value.as<uint64_t>();
        return true;
    }
    return false;
}

void setRpcId(JsonObject document, const RpcId& id) {
    switch (id.kind) {
        case RpcId::Kind::Null:
            document["id"] = nullptr;
            break;
        case RpcId::Kind::Signed:
            document["id"] = id.signed_value;
            break;
        case RpcId::Kind::Unsigned:
            document["id"] = id.unsigned_value;
            break;
        case RpcId::Kind::String:
            document["id"] = id.string_value;
            break;
    }
}

void makeRpcError(const RpcId& id, int32_t code, const std::string& message,
                  std::string& response) {
    JsonDocument document;
    document["jsonrpc"] = "2.0";
    setRpcId(document.as<JsonObject>(), id);
    JsonObject error = document["error"].to<JsonObject>();
    error["code"] = code;
    error["message"] = message;
    response.clear();
    serializeJson(document, response);
}

void makeRpcResult(const RpcId& id, JsonVariantConst result, std::string& response) {
    JsonDocument document;
    document["jsonrpc"] = "2.0";
    setRpcId(document.as<JsonObject>(), id);
    document["result"].set(result);
    response.clear();
    serializeJson(document, response);
}

void addPropertySchema(JsonObject destination, const McpProperty& property) {
    switch (property.type) {
        case McpPropertyType::Boolean:
            destination["type"] = "boolean";
            if (property.has_default) {
                destination["default"] = std::get<bool>(property.default_value);
            }
            break;
        case McpPropertyType::Integer:
            destination["type"] = "integer";
            if (property.has_default) {
                destination["default"] = std::get<int32_t>(property.default_value);
            }
            if (property.has_range) {
                destination["minimum"] = property.minimum;
                destination["maximum"] = property.maximum;
            }
            break;
        case McpPropertyType::String:
            destination["type"] = "string";
            if (property.has_default) {
                destination["default"] = std::get<std::string>(property.default_value);
            }
            break;
    }
}

void addToolSchema(JsonObject destination, const McpTool& tool) {
    destination["name"] = tool.name;
    destination["description"] = tool.description;
    JsonObject input_schema = destination["inputSchema"].to<JsonObject>();
    input_schema["type"] = "object";
    JsonObject properties = input_schema["properties"].to<JsonObject>();
    JsonArray required = input_schema["required"].to<JsonArray>();
    for (const auto& property : tool.properties) {
        JsonObject schema = properties[property.name].to<JsonObject>();
        addPropertySchema(schema, property);
        if (property.required) {
            required.add(property.name);
        }
    }
    if (required.size() == 0) {
        input_schema.remove("required");
    }
    if (tool.user_only) {
        JsonObject annotations = destination["annotations"].to<JsonObject>();
        JsonArray audience = annotations["audience"].to<JsonArray>();
        audience.add("user");
    }
}

bool parseArguments(const McpTool& tool, JsonObjectConst input,
                    std::vector<std::pair<std::string, McpValue>>& output,
                    std::string& error) {
    output.clear();
    for (const auto& property : tool.properties) {
        JsonVariantConst value = input[property.name];
        if (value.isNull()) {
            if (property.has_default) {
                output.emplace_back(property.name, property.default_value);
                continue;
            }
            if (property.required) {
                error = "Missing valid argument: " + property.name;
                return false;
            }
            continue;
        }

        switch (property.type) {
            case McpPropertyType::Boolean:
                if (!value.is<bool>()) {
                    error = "Argument is not boolean: " + property.name;
                    return false;
                }
                output.emplace_back(property.name, value.as<bool>());
                break;
            case McpPropertyType::Integer: {
                if (!value.is<int32_t>()) {
                    error = "Argument is not a 32-bit integer: " + property.name;
                    return false;
                }
                const int32_t integer = value.as<int32_t>();
                if (property.has_range &&
                    (integer < property.minimum || integer > property.maximum)) {
                    error = "Argument is outside its allowed range: " + property.name;
                    return false;
                }
                output.emplace_back(property.name, integer);
                break;
            }
            case McpPropertyType::String:
                {
                std::string string;
                if (!copyJsonString(value, string)) {
                    error = "Argument is not a string: " + property.name;
                    return false;
                }
                output.emplace_back(property.name, std::move(string));
                break;
                }
        }
    }
    error.clear();
    return true;
}

bool makeCallResult(const McpResult& result, JsonDocument& output, std::string& error) {
    JsonArray content = output["content"].to<JsonArray>();
    JsonObject item = content.add<JsonObject>();
    item["type"] = "text";
    switch (result.kind()) {
        case McpResult::Kind::Text:
            item["text"] = result.stringValue();
            break;
        case McpResult::Kind::Boolean:
            item["text"] = result.booleanValue() ? "true" : "false";
            break;
        case McpResult::Kind::Integer:
            item["text"] = std::to_string(result.integerValue());
            break;
        case McpResult::Kind::Json: {
            JsonDocument json;
            const DeserializationError json_error = deserializeJson(json, result.stringValue());
            if (json_error) {
                error = std::string("tool returned invalid JSON: ") + json_error.c_str();
                return false;
            }
            std::string serialized;
            serializeJson(json, serialized);
            item["text"] = serialized;
            break;
        }
    }
    output["isError"] = result.isError();
    error.clear();
    return true;
}

}  // namespace

McpProperty McpProperty::Boolean(const std::string& name) {
    return makeProperty(name, McpPropertyType::Boolean);
}

McpProperty McpProperty::Boolean(const std::string& name, bool default_value) {
    McpProperty property = Boolean(name);
    property.required = false;
    property.has_default = true;
    property.default_value = default_value;
    return property;
}

McpProperty McpProperty::Integer(const std::string& name) {
    return makeProperty(name, McpPropertyType::Integer);
}

McpProperty McpProperty::Integer(const std::string& name, int32_t minimum,
                                 int32_t maximum) {
    McpProperty property = Integer(name);
    property.has_range = true;
    property.minimum = minimum;
    property.maximum = maximum;
    return property;
}

McpProperty McpProperty::Integer(const std::string& name, int32_t default_value,
                                 int32_t minimum, int32_t maximum) {
    McpProperty property = Integer(name, minimum, maximum);
    property.required = false;
    property.has_default = true;
    property.default_value = default_value;
    return property;
}

McpProperty McpProperty::String(const std::string& name) {
    return makeProperty(name, McpPropertyType::String);
}

McpProperty McpProperty::String(const std::string& name,
                                const std::string& default_value) {
    McpProperty property = String(name);
    property.required = false;
    property.has_default = true;
    property.default_value = default_value;
    return property;
}

bool McpArguments::contains(const std::string& name) const {
    return findValue(values_, name) != nullptr;
}

bool McpArguments::getBool(const std::string& name, bool& value) const {
    const McpValue* found = findValue(values_, name);
    if (found == nullptr || !std::holds_alternative<bool>(*found)) {
        return false;
    }
    value = std::get<bool>(*found);
    return true;
}

bool McpArguments::getInt(const std::string& name, int32_t& value) const {
    const McpValue* found = findValue(values_, name);
    if (found == nullptr || !std::holds_alternative<int32_t>(*found)) {
        return false;
    }
    value = std::get<int32_t>(*found);
    return true;
}

bool McpArguments::getString(const std::string& name, std::string& value) const {
    const McpValue* found = findValue(values_, name);
    if (found == nullptr || !std::holds_alternative<std::string>(*found)) {
        return false;
    }
    value = std::get<std::string>(*found);
    return true;
}

McpResult McpResult::Text(const std::string& value) {
    McpResult result;
    result.kind_ = Kind::Text;
    result.string_value_ = value;
    return result;
}

McpResult McpResult::Boolean(bool value) {
    McpResult result;
    result.kind_ = Kind::Boolean;
    result.boolean_value_ = value;
    return result;
}

McpResult McpResult::Integer(int32_t value) {
    McpResult result;
    result.kind_ = Kind::Integer;
    result.integer_value_ = value;
    return result;
}

McpResult McpResult::Json(const std::string& value) {
    McpResult result;
    result.kind_ = Kind::Json;
    result.string_value_ = value;
    return result;
}

McpResult McpResult::Error(const std::string& message) {
    McpResult result = Text(message);
    result.is_error_ = true;
    return result;
}

McpServer::McpServer(std::string server_name, std::string server_version)
    : server_name_("xiaozhi-arduino"), server_version_("2.4.0") {
    setServerInfo(server_name, server_version);
}

bool McpServer::addTool(McpTool tool, std::string* error) {
    if (tools_.size() >= 64) {
        setError(error, "MCP tool limit (64) reached");
        return false;
    }
    if (!validName(tool.name, 128)) {
        setError(error, "tool name is empty, too long, or contains unsupported characters");
        return false;
    }
    if (tool.description.empty() || tool.description.size() > 1024) {
        setError(error, "tool description must contain 1..1024 characters");
        return false;
    }
    if (!tool.handler) {
        setError(error, "tool handler is empty");
        return false;
    }
    if (tool.properties.size() > 32) {
        setError(error, "a tool may define at most 32 properties");
        return false;
    }
    if (std::any_of(tools_.begin(), tools_.end(),
                    [&tool](const McpTool& existing) { return existing.name == tool.name; })) {
        setError(error, "tool already exists: " + tool.name);
        return false;
    }
    for (size_t index = 0; index < tool.properties.size(); ++index) {
        const McpProperty& property = tool.properties[index];
        if (!validName(property.name, 64)) {
            setError(error, "invalid property name: " + property.name);
            return false;
        }
        for (size_t previous = 0; previous < index; ++previous) {
            if (tool.properties[previous].name == property.name) {
                setError(error, "duplicate property name: " + property.name);
                return false;
            }
        }
        if (property.has_range) {
            if (property.type != McpPropertyType::Integer || property.minimum > property.maximum) {
                setError(error, "invalid integer range: " + property.name);
                return false;
            }
            if (property.has_default) {
                if (!std::holds_alternative<int32_t>(property.default_value)) {
                    setError(error, "integer property has a mismatched default: " + property.name);
                    return false;
                }
                const int32_t value = std::get<int32_t>(property.default_value);
                if (value < property.minimum || value > property.maximum) {
                    setError(error, "integer default is outside its range: " + property.name);
                    return false;
                }
            }
        }
        if (property.has_default) {
            const bool correct_type =
                (property.type == McpPropertyType::Boolean &&
                 std::holds_alternative<bool>(property.default_value)) ||
                (property.type == McpPropertyType::Integer &&
                 std::holds_alternative<int32_t>(property.default_value)) ||
                (property.type == McpPropertyType::String &&
                 std::holds_alternative<std::string>(property.default_value));
            if (!correct_type) {
                setError(error, "property default type mismatch: " + property.name);
                return false;
            }
            if (property.type == McpPropertyType::String &&
                std::get<std::string>(property.default_value).size() > 1024) {
                setError(error, "string default exceeds 1024 bytes: " + property.name);
                return false;
            }
        }
    }

    tools_.push_back(std::move(tool));
    setError(error, "");
    return true;
}

bool McpServer::removeTool(const std::string& name) {
    const auto iterator =
        std::find_if(tools_.begin(), tools_.end(),
                     [&name](const McpTool& tool) { return tool.name == name; });
    if (iterator == tools_.end()) {
        return false;
    }
    tools_.erase(iterator);
    return true;
}

void McpServer::clearTools() {
    tools_.clear();
}

void McpServer::setServerInfo(const std::string& name, const std::string& version) {
    server_name_ = name.empty() ? "xiaozhi-arduino" : name.substr(0, 128);
    server_version_ = version.empty() ? "2.4.0" : version.substr(0, 32);
}

void McpServer::setMaxResponseBytes(size_t bytes) {
    max_response_bytes_ = std::clamp<size_t>(bytes, 256, 16384);
}

void McpServer::setUserToolAuthorizer(McpUserToolAuthorizer authorizer) {
    user_tool_authorizer_ = std::move(authorizer);
}

bool McpServer::handle(const uint8_t* data, size_t size, std::string& response,
                       std::string& error) const {
    response.clear();
    if (data == nullptr || size == 0) {
        error = "MCP request is empty or exceeds the configured size limit";
        return false;
    }
    if (size > max_response_bytes_) {
        error = "MCP request exceeds the configured size limit";
        return false;
    }

    JsonDocument request;
    const DeserializationError json_error = deserializeJson(request, data, size);
    if (json_error) {
        makeRpcError({}, kParseError, "Parse error", response);
        error.clear();
        return true;
    }
    if (!request.is<JsonObject>()) {
        makeRpcError({}, kInvalidRequest, "Invalid Request", response);
        error.clear();
        return true;
    }

    return handle(request.as<JsonObjectConst>(), response, error);
}

bool McpServer::handle(JsonObjectConst root, std::string& response,
                       std::string& error) const {
    response.clear();
    if (measureJson(root) > max_response_bytes_) {
        error = "MCP request exceeds the configured size limit";
        return false;
    }
    std::string jsonrpc;
    std::string method;
    if (!copyJsonString(root["jsonrpc"], jsonrpc, 3) || jsonrpc != "2.0" ||
        !copyJsonString(root["method"], method, 128) || method.empty()) {
        makeRpcError({}, kInvalidRequest, "Invalid Request", response);
        error.clear();
        return true;
    }
    // Only an omitted id denotes a JSON-RPC notification. An explicit null id is a request and
    // receives a response whose id is null, as required for correlating the error/result.
    JsonVariantConst id_value = root["id"];
    if (id_value.isUnbound()) {
        error.clear();
        return true;
    }
    RpcId id;
    if (!parseRpcId(id_value, id)) {
        makeRpcError({}, kInvalidRequest, "Invalid Request", response);
        error.clear();
        return true;
    }
    JsonVariantConst params_value = root["params"];
    if (!params_value.isNull() && !params_value.is<JsonObjectConst>()) {
        makeRpcError(id, kInvalidParams, "Invalid params", response);
        error.clear();
        return true;
    }
    JsonObjectConst params = params_value.as<JsonObjectConst>();

    JsonDocument result;
    if (method == "initialize") {
        result["protocolVersion"] = "2024-11-05";
        result["capabilities"]["tools"].to<JsonObject>();
        result["serverInfo"]["name"] = server_name_;
        result["serverInfo"]["version"] = server_version_;
        makeRpcResult(id, result.as<JsonVariantConst>(), response);
    } else if (method == "tools/list") {
        std::string cursor;
        bool with_user_tools = false;
        if (!params.isNull()) {
            if (params["cursor"].is<const char*>()) {
                if (!copyJsonString(params["cursor"], cursor, 128)) {
                    makeRpcError(id, kInvalidParams, "Invalid cursor", response);
                    error.clear();
                    return true;
                }
            }
            if (params["withUserTools"].is<bool>()) {
                with_user_tools = params["withUserTools"].as<bool>();
            }
        }

        size_t start = 0;
        if (!cursor.empty()) {
            const auto found = std::find_if(
                tools_.begin(), tools_.end(),
                [&cursor](const McpTool& tool) { return tool.name == cursor; });
            if (found == tools_.end()) {
                makeRpcError(id, kInvalidParams, "Unknown cursor: " + cursor, response);
                error.clear();
                return true;
            }
            start = static_cast<size_t>(std::distance(tools_.begin(), found));
        }

        JsonArray list = result["tools"].to<JsonArray>();
        for (size_t index = start; index < tools_.size(); ++index) {
            const McpTool& tool = tools_[index];
            if (!with_user_tools && tool.user_only) {
                continue;
            }
            JsonObject schema = list.add<JsonObject>();
            addToolSchema(schema, tool);

            JsonDocument candidate;
            candidate["jsonrpc"] = "2.0";
            setRpcId(candidate.as<JsonObject>(), id);
            candidate["result"].set(result.as<JsonVariantConst>());
            std::string serialized;
            serializeJson(candidate, serialized);
            if (serialized.size() > max_response_bytes_) {
                list.remove(list.size() - 1);
                if (list.size() == 0) {
                    makeRpcError(id, kInternalError,
                                 "Tool schema exceeds the MCP response size limit", response);
                    error.clear();
                    return true;
                }
                result["nextCursor"] = tool.name;
                break;
            }
        }
        makeRpcResult(id, result.as<JsonVariantConst>(), response);
    } else if (method == "tools/call") {
        std::string tool_name;
        if (params.isNull() || !copyJsonString(params["name"], tool_name, 128) ||
            tool_name.empty()) {
            makeRpcError(id, kInvalidParams, "Missing tool name", response);
            error.clear();
            return true;
        }
        const auto found = std::find_if(
            tools_.begin(), tools_.end(),
            [&tool_name](const McpTool& tool) { return tool.name == tool_name; });
        if (found == tools_.end()) {
            makeRpcError(id, kInvalidParams, "Unknown tool: " + tool_name, response);
            error.clear();
            return true;
        }
        const McpTool tool = *found;
        const McpUserToolAuthorizer authorizer = user_tool_authorizer_;
        if (tool.user_only && (!authorizer || !authorizer(tool.name))) {
            makeRpcError(id, kUserAuthorizationRequired,
                         "User authorization required: " + tool.name, response);
            error.clear();
            return true;
        }
        JsonVariantConst arguments_value = params["arguments"];
        if (!arguments_value.isNull() && !arguments_value.is<JsonObjectConst>()) {
            makeRpcError(id, kInvalidParams, "Invalid arguments", response);
            error.clear();
            return true;
        }
        JsonObjectConst arguments_json = arguments_value.as<JsonObjectConst>();
        McpArguments arguments;
        std::string argument_error;
        if (!parseArguments(tool, arguments_json, arguments.values_, argument_error)) {
            makeRpcError(id, kInvalidParams, argument_error, response);
            error.clear();
            return true;
        }

        const McpResult call_result = tool.handler(arguments);
        if (call_result.stringValue().size() > max_response_bytes_) {
            makeRpcError(id, kInternalError, "Tool result exceeds the MCP response size limit",
                         response);
            error.clear();
            return true;
        }
        std::string result_error;
        if (!makeCallResult(call_result, result, result_error)) {
            makeRpcError(id, kInternalError, result_error, response);
        } else {
            makeRpcResult(id, result.as<JsonVariantConst>(), response);
        }
    } else {
        makeRpcError(id, kMethodNotFound, "Method not implemented: " + method, response);
    }

    if (response.size() > max_response_bytes_) {
        makeRpcError(id, kInternalError, "MCP response exceeds the configured size limit",
                     response);
    }
    error.clear();
    return true;
}

}  // namespace xiaozhi
