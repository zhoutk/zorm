#include <mcp/types.hpp>

namespace mcp {

std::optional<RequestId> decodeRequestId(const Json& v) {
  if (v.isString()) {
    return RequestId(std::string(v.toString()));
  }
  if (v.isNumber()) {
    // Must be integral: fractional ids violate the spec ("string or integer").
    const double d = v.toDouble();
    const int64_t i = v.toInt64();
    if (static_cast<double>(i) != d) return std::nullopt;
    return RequestId(i);
  }
  return std::nullopt;
}

Json encodeRequestId(const RequestId& id) {
  if (const auto* i = std::get_if<int64_t>(&id)) return Json(*i);
  return Json::str(std::get<std::string>(id));
}

std::string resultTypeOf(const Json& result) {
  if (!result.isObject()) return std::string(kResultTypeComplete);
  const Json rt = result[std::string(kResultTypeKey)];
  if (!rt.isString()) return std::string(kResultTypeComplete);
  return std::string(rt.toString());
}

// ---- Implementation ----------------------------------------------------------

Json encodeImplementation(const Implementation& impl) {
  Json j;
  j.add("name", Json::str(impl.name));
  if (!impl.title.empty()) j.add("title", Json::str(impl.title));
  j.add("version", Json::str(impl.version));
  if (!impl.description.empty()) j.add("description", Json::str(impl.description));
  if (!impl.websiteUrl.empty()) j.add("websiteUrl", Json::str(impl.websiteUrl));
  if (!impl.icons.isNull()) j.add("icons", impl.icons);
  return j;
}

std::optional<Implementation> decodeImplementation(const Json& v) {
  if (!v.isObject()) return std::nullopt;
  const Json name = v["name"];
  const Json version = v["version"];
  if (!name.isString() || !version.isString()) return std::nullopt;

  Implementation impl;
  impl.name = std::string(name.toString());
  impl.version = std::string(version.toString());

  const Json title = v["title"];
  if (title.isString()) impl.title = std::string(title.toString());
  const Json description = v["description"];
  if (description.isString()) impl.description = std::string(description.toString());
  const Json websiteUrl = v["websiteUrl"];
  if (websiteUrl.isString()) impl.websiteUrl = std::string(websiteUrl.toString());
  const Json icons = v["icons"];
  if (icons.isArray()) impl.icons = icons;
  return impl;
}

// ---- per-request metadata ------------------------------------------------------

McpResult<RequestMeta> decodeRequestMeta(const Json& params) {
  if (!params.isObject()) {
    return Err(McpErrorCode::InvalidParams, "request params must be an object");
  }
  const Json meta = params["_meta"];
  if (!meta.isObject()) {
    return Err(McpErrorCode::InvalidParams,
               "request is missing the required _meta object");
  }
  const Json pv = meta[std::string(kMetaProtocolVersion)];
  if (!pv.isString() || pv.toString().empty()) {
    return Err(McpErrorCode::InvalidParams,
               "request _meta is missing io.modelcontextprotocol/protocolVersion");
  }
  const Json caps = meta[std::string(kMetaClientCapabilities)];
  if (!caps.isObject()) {
    return Err(McpErrorCode::InvalidParams,
               "request _meta is missing io.modelcontextprotocol/clientCapabilities");
  }
  RequestMeta m;
  m.protocolVersion = std::string(pv.toString());
  m.clientCapabilities = caps;
  const Json info = meta[std::string(kMetaClientInfo)];
  if (info.isObject()) m.clientInfo = decodeImplementation(info);
  return Ok(std::move(m));
}

Json encodeRequestMeta(std::string_view protocolVersion,
                       const Json& clientCapabilities,
                       const std::optional<Implementation>& clientInfo) {
  Json meta;
  meta.add(std::string(kMetaProtocolVersion), Json::str(std::string(protocolVersion)));
  meta.add(std::string(kMetaClientCapabilities), clientCapabilities);
  if (clientInfo) meta.add(std::string(kMetaClientInfo), encodeImplementation(*clientInfo));
  Json params;
  params.add("_meta", std::move(meta));
  return params;
}

// ---- capability probes ---------------------------------------------------------

bool capabilitiesInclude(const Json& capabilities, std::string_view key) {
  if (!capabilities.isObject()) return false;
  const Json member = capabilities[std::string(key)];
  return member.isObject() || member.isTrue();
}

bool capabilitiesIncludeExtension(const Json& capabilities, std::string_view extensionId) {
  if (!capabilities.isObject()) return false;
  const Json extensions = capabilities["extensions"];
  return extensions.isObject() && extensions[std::string(extensionId)].isObject();
}

}  // namespace mcp
