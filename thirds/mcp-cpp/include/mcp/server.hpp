#pragma once
// ---------------------------------------------------------------------------
// mcp/server.hpp - MCP server runtime + builder.
//
// Design (specification 2026-07-28):
//  - stateless dispatch: every request validates its own params._meta
//    (protocolVersion + clientCapabilities required) and is served
//    independently; no session state exists (basic/index.md "Statelessness")
//  - version gate: a request whose _meta version is not in the supported set
//    is answered with UnsupportedProtocolVersionError (-32022) listing the
//    supported versions (basic/versioning.md)
//  - capability gating: calling tools/* without advertising the tools
//    capability is MethodNotFound (-32601) per the schema's
//    MethodNotFoundError documentation; an unknown TOOL is InvalidParams
//    (-32602)
//  - single-threaded dispatch loop for now (PLAN.md Update 1); cancellation
//    is a queryable flag keyed by request id, set by notifications/cancelled
//  - handlers own their result object; the server injects
//    resultType:"complete" when the handler forgot it (spec requires it on
//    every result)
//
// Builder mirrors the portable shape of the Tier-1 SDKs
// (Rust Server::builder().add_tool / TS McpServer.registerTool).
// ---------------------------------------------------------------------------

#include <mcp/error.hpp>
#include <mcp/json.hpp>
#include <mcp/jsonrpc.hpp>
#include <mcp/transport.hpp>
#include <mcp/types.hpp>

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace mcp {
namespace server {

// ---- handler surface ---------------------------------------------------------

// Per-request context handed to handlers. References stay valid for the
// duration of the handler call.
struct Ctx {
  const Json& params;              // full request params (validated object)
  const RequestMeta& meta;         // decoded params._meta
  const std::function<bool()>& isCancelled;  // true once notifications/cancelled names this request id
};

// A handler returns the full result object (including resultType when it
// matters; the server injects "complete" when absent), or an error that
// becomes the JSON-RPC error response verbatim.
using Handler = std::function<McpResult<Json>(Ctx&)>;

// ---- server --------------------------------------------------------------------

class Server {
 public:
  friend class Builder;

  // Runs the dispatch loop until the transport reaches EOF (graceful shutdown
  // signal for stdio). Returns the first transport error, if any.
  McpResult<void> run(ITransport& transport);

  // Processes one raw message and writes any response(s) to `out`.
  // Public so tests can drive the server over in-memory transports.
  void processLine(const std::string& line, ITransport& out);

  // --- introspection for tests ---
  const Json& serverCapabilities() const { return capabilities_; }
  const std::vector<std::string>& supportedVersions() const { return supported_; }

 private:
  Server() = default;

  struct Registry {
    std::vector<Json> definitions;             // wire form, served by *\/list
    std::map<std::string, Handler> handlers;   // key: name (or uri)
  };

  // handler dispatch, after meta/version/capability validation
  McpResult<Json> dispatchRequest(const JsonRpcRequest& req, const RequestMeta& meta,
                                  const std::function<bool()>& cancelled);

  McpResult<Json> handleDiscover(const RequestMeta& meta);
  McpResult<Json> handlePaginatedList(const Registry& reg, const Json& params,
                                      std::string_view arrayKey);
  McpResult<Json> handleToolCall(const JsonRpcRequest& req, const RequestMeta& meta,
                                 const std::function<bool()>& cancelled);

  bool versionSupported(std::string_view wire) const;

  Registry tools_;
  Registry resources_;
  Registry prompts_;
  Json capabilities_;  // starts as an empty object; keys added per feature
  std::vector<std::string> supported_;   // wire strings, used for -32022 data
  std::set<ProtocolVersion> supportedSet_;
  std::string instructions_;
  int pageSize_ = 0;          // 0 = no pagination on *\/list
  long discoverTtlMs_ = 0;    // 0 = discover result immediately stale
  std::string name_ = "mcp-cpp-server";
  std::string version_ = "0.1.0";

  // request id (string form) -> cancel flag
  std::map<std::string, std::shared_ptr<std::atomic_bool>> cancellations_;
};

// ---- builder ---------------------------------------------------------------------

class Builder {
 public:
  Builder& name(std::string n);
  Builder& version(std::string v);
  Builder& instructions(std::string text);
  Builder& supports(ProtocolVersion v);         // repeatable; default: Latest only
  Builder& pageSize(int n);                     // 0 disables *\/list pagination
  Builder& discoverTtlMs(long ms);

  // Registers a tool and advertises the tools capability.
  // `definition` must be a Tool object (name required, inputSchema recommended).
  Builder& tool(Json definition, Handler handler);
  // Registers a resource (uri-keyed) and advertises the resources capability.
  Builder& resource(Json definition, Handler readHandler);
  // Registers a prompt and advertises the prompts capability.
  Builder& prompt(Json definition, Handler handler);

  // Convenience: a text tool with no schema enforcement (M4 adds validation).
  Builder& simpleTool(std::string toolName, std::string description, Handler handler);

  Server build();

 private:
  Server server_;
};

}  // namespace server
}  // namespace mcp
