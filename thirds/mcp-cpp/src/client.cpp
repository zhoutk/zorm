#include <mcp/client.hpp>

#include "base64.h"  // x-mcp-header value encoding

#include <utility>

namespace mcp {
namespace client {
namespace {

RequestId nextRequestId(int64_t& counter) { return RequestId(counter++); }

}  // namespace

Client::Client(std::string clientName, std::string clientVersion, Json clientCapabilities)
    : clientName_(std::move(clientName)),
      clientVersion_(std::move(clientVersion)),
      clientCapabilities_(std::move(clientCapabilities)) {}

void Client::connect(ITransport& transport) { transport_ = &transport; }

void Client::setProtocolVersion(ProtocolVersion v) { protocolVersion_ = std::string(toString(v)); }

void Client::onNotification(NotificationSink sink) { onNotification_ = std::move(sink); }

void Client::onSubscriptionClosed(std::function<void(const std::string& subscriptionId)> cb) {
  onSubscriptionClosed_ = std::move(cb);
}

namespace {

std::string idToString(const RequestId& id) {
  if (const auto* i = std::get_if<int64_t>(&id)) return std::to_string(*i);
  return std::get<std::string>(id);
}

}  // namespace

// ---- request plumbing -------------------------------------------------------

Json Client::buildParams(const Json& extraParams) const {
  Json params = encodeRequestMeta(
      protocolVersion_, clientCapabilities_,
      Implementation{clientName_, std::string(), clientVersion_, std::string(), std::string(),
                     Json(nullptr)});
  if (extraParams.isObject()) {
    for (const auto& item : extraParams.getAllKeys().toVector()) {
      params.add(item.toString(), extraParams[item.toString()]);
    }
  }
  return params;
}

McpResult<Json> Client::request(std::string_view method, const Json& extraParams) {
  if (transport_ == nullptr) {
    return Err(McpErrorCode::InternalError, "client is not connected");
  }
  const RequestId id = nextRequestId(nextId_);
  const std::string idString = idToString(id);
  const std::string line = encodeRequest(id, method, buildParams(extraParams));
  auto wrote = transport_->write(line);
  if (!wrote.has_value()) return Err(wrote.error());

  // Read until the response with our id arrives; notifications en route are
  // delivered to the callback, never swallowed.
  while (true) {
    auto raw = transport_->read();
    if (!raw.has_value()) return Err(raw.error());
    if (!raw->has_value()) {
      return Err(McpErrorCode::InternalError, "connection closed by peer before response");
    }
    const ParsedMessage parsed = parseMessage(**raw);
    if (const auto* notification = std::get_if<JsonRpcNotification>(&parsed)) {
      // subscribe() resolves on the server's acknowledgment for this request.
      if (awaitAck_ && notification->method == "notifications/subscriptions/acknowledged") {
        const Json subId =
            notification->params["_meta"][std::string(kMetaSubscriptionId)];
        if (subId.isString() || subId.isNumber()) {
          awaitAck_ = false;
          if (subId.toString() == idString) return Ok(notification->params);
        }
      }
      if (onNotification_) onNotification_(*notification);
      continue;
    }
    if (const auto* invalid = std::get_if<InvalidMessage>(&parsed)) {
      return Err(invalid->code, invalid->message);
    }
    if (const auto* err = std::get_if<JsonRpcErrorResponse>(&parsed)) {
      if (err->id && *err->id == id) {
        McpError e;
        e.code = static_cast<McpErrorCode>(err->error.code);
        e.message = err->error.message;
        e.data = err->error.data;
        return Err(std::move(e));
      }
      continue;  // response to someone else (should not happen; skip defensively)
    }
    if (const auto* res = std::get_if<JsonRpcResult>(&parsed)) {
      if (res->id == id) return Ok(res->result);
      // A result carrying a subscriptionId closes that subscription
      // gracefully (basic/patterns/subscriptions.md "Graceful Closure").
      const Json subId = res->result["_meta"][std::string(kMetaSubscriptionId)];
      if (onSubscriptionClosed_ && (subId.isString() || subId.isNumber())) {
        onSubscriptionClosed_(std::string(subId.toString()));
      }
      continue;
    }
    // Requests arriving at a client: not supported by this runtime (MRTR is
    // result-embedded); ignore.
  }
}

// ---- protocol surface --------------------------------------------------------

McpResult<Json> Client::discover() { return request("server/discover", Json(nullptr)); }

McpResult<Json> Client::listTools(const std::string& cursor) {
  Json extra;
  if (!cursor.empty()) extra.add("cursor", Json::str(cursor));
  auto result = request("tools/list", extra);
  if (result.has_value()) {
    // Cache definitions for x-mcp-header mirroring.
    toolSchemas_.clear();
    const Json tools = (*result)["tools"];
    if (tools.isArray()) {
      for (const auto& item : tools.toVector()) {
        const Json name = item["name"];
        if (name.isString()) toolSchemas_[std::string(name.toString())] = item;
      }
    }
  }
  return result;
}

McpResult<Json> Client::callTool(const std::string& name, const Json& arguments,
                                 const Json& inputResponses) {
  mirrorHeaderParams(name, arguments);
  Json extra;
  extra.add("name", Json::str(name));
  if (!arguments.isNull()) extra.add("arguments", arguments);
  if (!inputResponses.isNull()) extra.add("inputResponses", inputResponses);
  return request("tools/call", extra);
}

McpResult<Json> Client::readResource(const std::string& uri) {
  Json extra;
  extra.add("uri", Json::str(uri));
  return request("resources/read", extra);
}

McpResult<Json> Client::getPrompt(const std::string& name, const Json& arguments) {
  Json extra;
  extra.add("name", Json::str(name));
  if (arguments.isObject()) extra.add("arguments", arguments);
  return request("prompts/get", extra);
}

McpResult<Json> Client::subscribe(const Json& filter) {
  awaitAck_ = true;
  auto r = request("subscriptions/listen", filter);
  awaitAck_ = false;
  return r;
}

namespace {

// Collects values at properties-chains annotated with "x-mcp-header".
// Only statically reachable chains qualify (properties keys only).
void collectHeaderParams(const Json& schema, const Json& args,
                         const std::vector<std::string>& path,
                         std::vector<std::pair<std::string, std::string>>& out) {
  if (!schema.isObject() || !args.isObject()) return;
  const Json properties = schema["properties"];
  if (!properties.isObject()) return;
  for (const auto& key : properties.getAllKeys().toVector()) {
    const std::string keyStr(key.toString());
    const Json prop = properties[keyStr];
    const Json annotation = prop["x-mcp-header"];
    if (annotation.isString() && !annotation.toString().empty()) {
      // Read the value at this exact path in the arguments.
      Json node = args;
      for (const auto& seg : path) node = node[seg];
      const Json value = node[keyStr];
      if (value.isError() || value.isNull()) continue;  // absent -> omit header
      std::string text;
      if (value.isString()) text = std::string(value.toString());
      else if (value.isNumber()) text = std::to_string(value.toInt64());
      else if (value.isFalse()) text = "false";
      else if (value.isTrue()) text = "true";
      if (!text.empty()) out.emplace_back(std::string(annotation.toString()), std::move(text));
    }
    // Descend through properties chains only.
    std::vector<std::string> nextPath = path;
    nextPath.push_back(keyStr);
    collectHeaderParams(prop, args[keyStr], nextPath, out);
  }
}

}  // namespace

void Client::mirrorHeaderParams(const std::string& toolName, const Json& arguments) {
  // Clear headers mirrored for the previous call.
  for (const auto& h : lastParamHeaders_) transport_->setHeader(h, "");
  lastParamHeaders_.clear();

  auto it = toolSchemas_.find(toolName);
  if (it == toolSchemas_.end()) return;  // unknown tool -> nothing to mirror
  const Json inputSchema = it->second["inputSchema"];
  if (!inputSchema.isObject()) return;

  std::vector<std::pair<std::string, std::string>> params;
  collectHeaderParams(inputSchema, arguments, {}, params);
  for (auto& [headerName, value] : params) {
    const std::string wireName = "Mcp-Param-" + headerName;
    transport_->setHeader(wireName, detail::encodeHeaderValue(value));
    lastParamHeaders_.push_back(wireName);
  }
}

McpResult<void> Client::cancel(const RequestId& requestId, const std::string& reason) {
  if (transport_ == nullptr) {
    return Err(McpErrorCode::InternalError, "client is not connected");
  }
  Json params;
  params.add("requestId", encodeRequestId(requestId));
  if (!reason.empty()) params.add("reason", Json::str(reason));
  const std::string line = encodeNotification("notifications/cancelled", params);
  return transport_->write(line);
}

}  // namespace client
}  // namespace mcp
