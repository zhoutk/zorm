#include <mcp/jsonrpc.hpp>

#include <cmath>
#include <utility>

namespace mcp {
namespace {

Json parseJsonText(std::string_view text) {
  // zjson parses from std::string; string_view input is copied once here.
  // (Parse hot path: revisit with a zero-copy view overload in M4.)
  std::string owned(text);
  std::string err;
  Json doc = Json::ParseJson(std::move(owned), err);
  return doc;
}

bool hasMember(const Json& obj, std::string_view key) {
  return obj.isObject() && obj.contains(std::string(key));
}

// The JSON-RPC envelope keys, fetched once as Json values.
struct Envelope {
  Json jsonrpc;
  Json id;
  Json method;
  Json params;
  Json result;
  Json error;
};

Envelope readEnvelope(const Json& doc) {
  Envelope e;
  e.jsonrpc = member(doc, "jsonrpc");
  e.id = member(doc, "id");
  e.method = member(doc, "method");
  e.params = member(doc, "params");
  e.result = member(doc, "result");
  e.error = member(doc, "error");
  return e;
}

// A request/notification whose params member is present but not an object is
// rejected: every MCP method takes object params (schema.ts RequestParams).
// An explicit null is normalized to absent.
Json normalizeParams(const Json& params) {
  if (params.isObject()) return params;
  return Json(nullptr);
}

InvalidMessage invalid(std::string_view message,
                       McpErrorCode code = McpErrorCode::InvalidRequest,
                       std::optional<RequestId> id = std::nullopt) {
  return InvalidMessage{std::move(id), code, std::string(message)};
}

}  // namespace

ParsedMessage parseMessage(std::string_view text) {
  const Json doc = parseJsonText(text);
  if (doc.isError()) {
    return invalid("Parse error: message is not valid JSON", McpErrorCode::ParseError);
  }
  if (!doc.isObject()) {
    // Arrays would be JSON-RPC batches; batching is not part of MCP.
    return invalid("Invalid request: expected a single JSON-RPC message object");
  }

  const Envelope e = readEnvelope(doc);

  if (!e.jsonrpc.isString() || e.jsonrpc.toString() != "2.0") {
    return invalid("Invalid request: jsonrpc member must be the string \"2.0\"");
  }

  const bool hasMethod = hasMember(doc, "method");
  const bool hasId = hasMember(doc, "id");

  if (hasMethod) {
    if (!e.method.isString() || e.method.toString().empty()) {
      return invalid("Invalid request: method must be a non-empty string");
    }

    if (!hasId) {
      // Notification: id member absent. (A present-but-null id member takes
      // the request branch below and is rejected there - MCP forbids null ids.)
      JsonRpcNotification n;
      n.method = std::string(e.method.toString());
      n.params = normalizeParams(e.params);
      return ParsedMessage(std::move(n));
    }

    // Request: id string|integer, never null/bool/fractional.
    auto id = decodeRequestId(e.id);
    if (!id) {
      return invalid("Invalid request: id must be a string or an integer, never null");
    }
    if (hasMember(doc, "params") && !e.params.isObject() && !e.params.isNull()) {
      return invalid("Invalid request: params must be an object", McpErrorCode::InvalidRequest,
                     std::move(*id));
    }
    JsonRpcRequest r;
    r.id = std::move(*id);
    r.method = std::string(e.method.toString());
    r.params = normalizeParams(e.params);
    return ParsedMessage(std::move(r));
  }

  // No method: must be a response (result or error).
  const bool hasResult = hasMember(doc, "result");
  const bool hasError = hasMember(doc, "error");
  if (hasResult == hasError) {
    return invalid("Invalid request: response must have exactly one of result/error");
  }

  std::optional<RequestId> id;
  if (hasId) {
    id = decodeRequestId(e.id);
    if (!id) {
      return invalid("Invalid request: id must be a string or an integer, never null");
    }
  } else if (hasResult) {
    // A result response MUST carry its request id to be correlatable; only
    // error responses may omit it (id unreadable), per the schema's
    // `id?: RequestId` on JSONRPCErrorResponse.
    return invalid("Invalid request: result response must carry its request id");
  }

  if (hasResult) {
    JsonRpcResult res;
    res.id = std::move(*id);
    res.result = e.result;
    return ParsedMessage(std::move(res));
  }

  // Error response: { code: integer, message: string, data?: any }.
  if (!e.error.isObject()) {
    return invalid("Invalid request: error must be an object", McpErrorCode::InvalidRequest,
                   std::move(id));
  }
  const Json code = member(e.error, "code");
  const Json message = member(e.error, "message");
  if (!code.isNumber() || static_cast<double>(code.toInt64()) != code.toDouble()) {
    return invalid("Invalid request: error.code must be an integer", McpErrorCode::InvalidRequest,
                   std::move(id));
  }
  if (!message.isString()) {
    return invalid("Invalid request: error.message must be a string", McpErrorCode::InvalidRequest,
                   std::move(id));
  }
  JsonRpcErrorResponse errRes;
  errRes.id = std::move(id);
  errRes.error.code = code.toInt64();
  errRes.error.message = std::string(message.toString());
  const Json data = member(e.error, "data");
  if (hasMember(e.error, "data")) errRes.error.data = data;
  return ParsedMessage(std::move(errRes));
}

bool isInvalid(const ParsedMessage& m) {
  return std::holds_alternative<InvalidMessage>(m);
}

bool isRequest(const ParsedMessage& m) {
  return std::holds_alternative<JsonRpcRequest>(m);
}

// ---- encoding -------------------------------------------------------------------

namespace {

Json buildEnvelope() {
  Json j;
  j.add("jsonrpc", Json::str("2.0"));
  return j;
}

}  // namespace

std::string encodeRequest(const RequestId& id, std::string_view method, const Json& params) {
  Json j = buildEnvelope();
  j.add("id", encodeRequestId(id));
  j.add("method", Json::str(std::string(method)));
  if (!params.isNull()) j.add("params", params);
  return j.toString();
}

std::string encodeNotification(std::string_view method, const Json& params) {
  Json j = buildEnvelope();
  j.add("method", Json::str(std::string(method)));
  if (!params.isNull()) j.add("params", params);
  return j.toString();
}

std::string encodeResultResponse(const RequestId& id, const Json& result) {
  Json j = buildEnvelope();
  j.add("id", encodeRequestId(id));
  j.add("result", result);
  return j.toString();
}

std::string encodeErrorResponse(const std::optional<RequestId>& id,
                                const JsonRpcErrorValue& error) {
  Json e;
  e.add("code", Json(static_cast<int64_t>(error.code)));
  e.add("message", Json::str(error.message));
  if (!error.data.isNull()) e.add("data", error.data);

  Json j = buildEnvelope();
  if (id) j.add("id", encodeRequestId(*id));
  j.add("error", std::move(e));
  return j.toString();
}

JsonRpcErrorValue parseErrorValue(std::string_view detail) {
  JsonRpcErrorValue v;
  v.code = static_cast<int32_t>(McpErrorCode::ParseError);
  v.message = std::string(detail);
  return v;
}

JsonRpcErrorValue invalidRequestValue(std::string_view detail) {
  JsonRpcErrorValue v;
  v.code = static_cast<int32_t>(McpErrorCode::InvalidRequest);
  v.message = std::string(detail);
  return v;
}

}  // namespace mcp
