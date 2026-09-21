#include <mcp/server.hpp>

#include <iostream>
#include <utility>

namespace mcp {
namespace server {
namespace {

// Method -> capability key required to serve it (schema: MethodNotFoundError
// documents that un-advertised features answer MethodNotFound).
struct MethodRoute {
  std::string_view method;
  std::string_view capability;  // empty = core, always available
  std::string_view registryKey; // "tools" / "resources" / "prompts" / ""
};

constexpr MethodRoute kRoutes[] = {
    {"server/discover", "", ""},
    {"tools/list", "tools", "tools"},
    {"tools/call", "tools", "tools"},
    {"resources/list", "resources", "resources"},
    {"resources/read", "resources", "resources"},
    {"resources/templates/list", "resources", "resources"},
    {"prompts/list", "prompts", "prompts"},
    {"prompts/get", "prompts", "prompts"},
};

std::string idToString(const RequestId& id) {
  if (const auto* i = std::get_if<int64_t>(&id)) return std::to_string(*i);
  return std::get<std::string>(id);
}

JsonRpcErrorValue toErrorValue(McpErrorCode code, std::string message) {
  JsonRpcErrorValue v;
  v.code = static_cast<int32_t>(code);
  v.message = std::move(message);
  return v;
}

JsonRpcErrorValue toErrorValue(const McpError& e) {
  return toErrorValue(e.code, e.message);
}

const MethodRoute* findRoute(std::string_view method) {
  for (const auto& route : kRoutes) {
    if (route.method == method) return &route;
  }
  return nullptr;
}

}  // namespace

// ---- dispatch loop -----------------------------------------------------------

McpResult<void> Server::run(ITransport& transport) {
  while (true) {
    auto line = transport.read();
    if (!line.has_value()) return Err(line.error());  // transport failure
    if (!line->has_value()) return Ok();              // EOF: graceful shutdown
    processLine(**line, transport);
  }
}

void Server::processLine(const std::string& line, ITransport& out) {
  const ParsedMessage parsed = parseMessage(line);

  if (const auto* invalid = std::get_if<InvalidMessage>(&parsed)) {
    JsonRpcErrorValue v = toErrorValue(invalid->code, invalid->message);
    (void)out.write(encodeErrorResponse(invalid->id, v));
    return;
  }

  if (const auto* notification = std::get_if<JsonRpcNotification>(&parsed)) {
    if (notification->method == "notifications/cancelled") {
      const Json requestId = notification->params["requestId"];
      auto id = decodeRequestId(requestId);
      if (id) {
        auto flag = std::make_shared<std::atomic_bool>(true);
        cancellations_[idToString(*id)] = flag;
      }
    }
    // Notifications MUST NOT be answered; unknown ones are ignored.
    return;
  }

  // Responses arriving at a server: this runtime never initiates requests
  // (server-to-client interaction travels inside InputRequiredResult), so
  // any response is unexpected - ignore it rather than corrupt the stream.
  if (!std::holds_alternative<JsonRpcRequest>(parsed)) return;

  const JsonRpcRequest& req = std::get<JsonRpcRequest>(parsed);

  // Stateless protocol: every request carries its own metadata.
  auto meta = decodeRequestMeta(req.params);
  if (!meta.has_value()) {
    (void)out.write(encodeErrorResponse(req.id, toErrorValue(meta.error())));
    return;
  }

  if (!versionSupported(meta->protocolVersion)) {
    JsonRpcErrorValue v = toErrorValue(McpErrorCode::UnsupportedProtocolVersion,
                                       "Unsupported protocol version");
    Json data;
    Json versions = Json("[]");
    for (const auto& wire : supported_) versions.push_back(Json::str(std::string(wire)));
    data.add("supported", std::move(versions));
    data.add("requested", Json::str(meta->protocolVersion));
    v.data = std::move(data);
    (void)out.write(encodeErrorResponse(req.id, v));
    return;
  }

  const auto* route = findRoute(req.method);
  if (route == nullptr) {
    (void)out.write(encodeErrorResponse(
        req.id, toErrorValue(McpErrorCode::MethodNotFound,
                             "Method not found: " + req.method)));
    return;
  }

  // Cancellation flag for this request. Reuse an existing slot: if a
  // notifications/cancelled arrived before the request itself (out-of-order
  // delivery), the request must observe cancelled=true, not a fresh flag.
  std::shared_ptr<std::atomic_bool>& cancelFlag = cancellations_[idToString(req.id)];
  if (!cancelFlag) cancelFlag = std::make_shared<std::atomic_bool>(false);
  const std::function<bool()> cancelled = [cancelFlag] { return cancelFlag->load(); };

  McpResult<Json> result = dispatchRequest(req, *meta, cancelled);
  if (result.has_value()) {
    // Spec: servers implementing this version MUST include resultType on
    // every result. Handlers that forgot it get "complete".
    if (result->isObject() && !result->contains(std::string(kResultTypeKey))) {
      result->add(std::string(kResultTypeKey), Json::str(std::string(kResultTypeComplete)));
    }
    (void)out.write(encodeResultResponse(req.id, *result));
  } else {
    (void)out.write(encodeErrorResponse(req.id, toErrorValue(result.error())));
  }
  cancellations_.erase(idToString(req.id));
}

McpResult<Json> Server::dispatchRequest(const JsonRpcRequest& req, const RequestMeta& meta,
                                        const std::function<bool()>& cancelled) {
  const MethodRoute* route = findRoute(req.method);

  // Capability gate: the server must have advertised the feature.
  if (!route->capability.empty() && !capabilitiesInclude(capabilities_, route->capability)) {
    return Err(McpErrorCode::MethodNotFound,
               "Method not found: " + req.method + " (capability not advertised)");
  }

  if (req.method == "server/discover") return handleDiscover(meta);
  if (req.method == "tools/list") return handlePaginatedList(tools_, req.params, "tools");
  if (req.method == "tools/call") return handleToolCall(req, meta, cancelled);
  if (req.method == "resources/list") return handlePaginatedList(resources_, req.params, "resources");
  if (req.method == "prompts/list") return handlePaginatedList(prompts_, req.params, "prompts");

  if (req.method == "resources/read") {
    const Json uri = req.params["uri"];
    if (!uri.isString()) return Err(McpErrorCode::InvalidParams, "resources/read requires a string uri");
    auto it = resources_.handlers.find(std::string(uri.toString()));
    if (it == resources_.handlers.end()) {
      return Err(McpErrorCode::InvalidParams, "Unknown resource: " + std::string(uri.toString()));
    }
    Ctx ctx{req.params, meta, cancelled};
    return it->second(ctx);
  }

  if (req.method == "prompts/get") {
    const Json name = req.params["name"];
    if (!name.isString()) return Err(McpErrorCode::InvalidParams, "prompts/get requires a string name");
    auto it = prompts_.handlers.find(std::string(name.toString()));
    if (it == prompts_.handlers.end()) {
      return Err(McpErrorCode::InvalidParams, "Unknown prompt: " + std::string(name.toString()));
    }
    Ctx ctx{req.params, meta, cancelled};
    return it->second(ctx);
  }

  return Err(McpErrorCode::MethodNotFound, "Method not found: " + req.method);
}

// ---- per-method handlers --------------------------------------------------------

McpResult<Json> Server::handleDiscover(const RequestMeta& /*meta*/) {
  Json result;
  result.add(std::string(kResultTypeKey), Json::str(std::string(kResultTypeComplete)));
  Json versions = Json("[]");
  for (const auto& wire : supported_) versions.push_back(Json::str(std::string(wire)));
  result.add("supportedVersions", std::move(versions));
  result.add("capabilities", capabilities_);
  if (!instructions_.empty()) result.add("instructions", Json::str(instructions_));
  result.add("ttlMs", Json(static_cast<int64_t>(discoverTtlMs_)));
  result.add("cacheScope", Json::str("public"));
  Json meta;
  Json info;
  info.add("name", Json::str(name_));
  info.add("version", Json::str(version_));
  meta.add(std::string(kMetaServerInfo), std::move(info));
  result.add("_meta", std::move(meta));
  return Ok(std::move(result));
}

McpResult<Json> Server::handlePaginatedList(const Registry& reg, const Json& params,
                                            std::string_view arrayKey) {
  Json result;
  result.add(std::string(kResultTypeKey), Json::str(std::string(kResultTypeComplete)));

  const size_t total = reg.definitions.size();
  size_t offset = 0;
  const Json cursor = params["cursor"];
  if (cursor.isString()) {
    // Opaque token: this registry encodes the plain offset. Unknown cursors
    // are rejected rather than silently restarting.
    const std::string c(cursor.toString());
    size_t parsed = 0;
    bool numeric = !c.empty();
    for (const char ch : c) {
      if (ch < '0' || ch > '9') { numeric = false; break; }
      parsed = parsed * 10 + static_cast<size_t>(ch - '0');
    }
    if (!numeric || parsed > total) {
      return Err(McpErrorCode::InvalidParams, "Invalid cursor");
    }
    offset = parsed;
  }

  Json arr = Json("[]");
  const size_t limit = (pageSize_ > 0) ? static_cast<size_t>(pageSize_) : total;
  for (size_t i = offset; i < total && i - offset < limit; ++i) {
    arr.push_back(reg.definitions[i]);
  }
  result.add(std::string(arrayKey), std::move(arr));
  if (pageSize_ > 0 && offset + limit < total) {
    result.add("nextCursor", Json::str(std::to_string(offset + limit)));
  }
  return Ok(std::move(result));
}

McpResult<Json> Server::handleToolCall(const JsonRpcRequest& req, const RequestMeta& meta,
                                       const std::function<bool()>& cancelled) {
  const Json name = req.params["name"];
  if (!name.isString()) {
    return Err(McpErrorCode::InvalidParams, "tools/call requires a string name");
  }
  auto it = tools_.handlers.find(std::string(name.toString()));
  if (it == tools_.handlers.end()) {
    return Err(McpErrorCode::InvalidParams, "Unknown tool: " + std::string(name.toString()));
  }
  Ctx ctx{req.params, meta, cancelled};
  return it->second(ctx);
}


bool Server::versionSupported(std::string_view wire) const {
  const auto v = fromString(wire);
  return v.has_value() && supportedSet_.count(*v) > 0;
}

// ---- builder ----------------------------------------------------------------------

Builder& Builder::name(std::string n) { server_.name_ = std::move(n); return *this; }
Builder& Builder::version(std::string v) { server_.version_ = std::move(v); return *this; }
Builder& Builder::instructions(std::string text) { server_.instructions_ = std::move(text); return *this; }
Builder& Builder::pageSize(int n) { server_.pageSize_ = n; return *this; }
Builder& Builder::discoverTtlMs(long ms) { server_.discoverTtlMs_ = ms; return *this; }

Builder& Builder::supports(ProtocolVersion v) {
  if (server_.supportedSet_.empty()) {
    // First explicit choice replaces the implicit Latest-only default.
    server_.supportedSet_.clear();
    server_.supported_.clear();
  }
  server_.supportedSet_.insert(v);
  server_.supported_.emplace_back(std::string(toString(v)));
  return *this;
}

Builder& Builder::tool(Json definition, Handler handler) {
  const Json name = definition["name"];
  if (!name.isString()) {
    std::cerr << "[mcp-cpp] builder: tool definition without name ignored\n";
    return *this;
  }
  const std::string key(name.toString());
  server_.tools_.definitions.push_back(std::move(definition));
  server_.tools_.handlers.emplace(key, std::move(handler));
  // Advertise the capability if not present yet.
  if (!capabilitiesInclude(server_.capabilities_, "tools")) {
    Json empty;
    server_.capabilities_.add("tools", std::move(empty));
  }
  return *this;
}

Builder& Builder::resource(Json definition, Handler readHandler) {
  const Json uri = definition["uri"];
  if (!uri.isString()) {
    std::cerr << "[mcp-cpp] builder: resource definition without uri ignored\n";
    return *this;
  }
  const std::string key(uri.toString());
  server_.resources_.definitions.push_back(std::move(definition));
  server_.resources_.handlers.emplace(key, std::move(readHandler));
  if (!capabilitiesInclude(server_.capabilities_, "resources")) {
    Json empty;
    server_.capabilities_.add("resources", std::move(empty));
  }
  return *this;
}

Builder& Builder::prompt(Json definition, Handler handler) {
  const Json name = definition["name"];
  if (!name.isString()) {
    std::cerr << "[mcp-cpp] builder: prompt definition without name ignored\n";
    return *this;
  }
  const std::string key(name.toString());
  server_.prompts_.definitions.push_back(std::move(definition));
  server_.prompts_.handlers.emplace(key, std::move(handler));
  if (!capabilitiesInclude(server_.capabilities_, "prompts")) {
    Json empty;
    server_.capabilities_.add("prompts", std::move(empty));
  }
  return *this;
}

Builder& Builder::simpleTool(std::string toolName, std::string description, Handler handler) {
  Json schema = Json(R"({"type":"object"})");
  Json def;
  def.add("name", Json::str(toolName));
  if (!description.empty()) def.add("description", Json::str(description));
  def.add("inputSchema", std::move(schema));
  return tool(std::move(def), std::move(handler));
}

Server Builder::build() {
  if (server_.supportedSet_.empty()) {
    supports(ProtocolVersion::Latest);
  }
  // Rebuild the supported list in enum order for a stable wire form.
  server_.supported_.clear();
  for (const auto v : {ProtocolVersion::V2024_11_05, ProtocolVersion::V2025_03_26,
                       ProtocolVersion::V2025_06_18, ProtocolVersion::V2025_11_25,
                       ProtocolVersion::V2026_07_28}) {
    if (server_.supportedSet_.count(v) > 0) {
      server_.supported_.emplace_back(std::string(toString(v)));
    }
  }
  return server_;  // copy: registries and Json are value types
}

}  // namespace server
}  // namespace mcp
