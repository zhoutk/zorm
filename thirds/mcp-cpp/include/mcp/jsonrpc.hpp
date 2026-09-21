#pragma once
// ---------------------------------------------------------------------------
// mcp/jsonrpc.hpp - JSON-RPC 2.0 message model, MCP-flavored.
//
// Differences from stock JSON-RPC 2.0 (specification 2026-07-28,
// basic/index.md "Messages"):
//   * request ids are string|integer and MUST NOT be null
//   * notifications MUST NOT carry an id
//   * batching is NOT supported (removed in 2025-06-18); arrays are rejected
//   * result responses carry a polymorphic `resultType` (absent = "complete")
//   * error responses omit the id when it could not be read (the schema has
//     no null id)
//
// parseMessage never throws and never fails: any input yields a ParsedMessage.
// Unparseable or protocol-invalid input decodes to InvalidMessage carrying
// everything needed to answer with a JSON-RPC error response.
// ---------------------------------------------------------------------------

#include <mcp/error.hpp>
#include <mcp/json.hpp>
#include <mcp/types.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>

namespace mcp {

// ---- incoming message shapes -------------------------------------------------

struct JsonRpcRequest {
  RequestId id;
  std::string method;
  Json params = Json(nullptr);  // Null when absent
};

struct JsonRpcNotification {
  std::string method;
  Json params = Json(nullptr);  // Null when absent
};

struct JsonRpcErrorValue {
  int32_t code = 0;
  std::string message;
  Json data = Json(nullptr);  // Null when absent
};

struct JsonRpcResult {  // a success response
  RequestId id;
  Json result;
};

struct JsonRpcErrorResponse {  // an error response from the peer
  std::optional<RequestId> id;  // absent when the id could not be read
  JsonRpcErrorValue error;
};

// Parse/protocol failure. `id` is set when a well-formed id was recoverable
// (else the error response must omit the id).
struct InvalidMessage {
  std::optional<RequestId> id;
  McpErrorCode code = McpErrorCode::ParseError;
  std::string message;
};

using ParsedMessage =
    std::variant<JsonRpcRequest, JsonRpcNotification, JsonRpcResult,
                 JsonRpcErrorResponse, InvalidMessage>;

// Strict decoder. `text` may be any wire bytes.
ParsedMessage parseMessage(std::string_view text);

// Convenience probes.
bool isInvalid(const ParsedMessage& m);
bool isRequest(const ParsedMessage& m);

// ---- outgoing message encoding -------------------------------------------------

// { jsonrpc, id, method, params? } - params omitted when null.
std::string encodeRequest(const RequestId& id, std::string_view method, const Json& params);

// { jsonrpc, method, params? } - never an id.
std::string encodeNotification(std::string_view method, const Json& params);

// { jsonrpc, id, result } - result must already carry its resultType.
std::string encodeResultResponse(const RequestId& id, const Json& result);

// { jsonrpc, id?, error:{ code, message, data? } } - id omitted when unset.
std::string encodeErrorResponse(const std::optional<RequestId>& id,
                                const JsonRpcErrorValue& error);

// Builds the standard error values for the two cases every endpoint needs.
JsonRpcErrorValue parseErrorValue(std::string_view detail);
JsonRpcErrorValue invalidRequestValue(std::string_view detail);

}  // namespace mcp
