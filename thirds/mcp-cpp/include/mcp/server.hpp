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
#include <mutex>
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

// ---- subscriptions -----------------------------------------------------------
// The filter mirrors the spec's Notification Filter object; every field the
// server honors is echoed in the acknowledgment.

struct SubscriptionFilter {
  bool toolsListChanged = false;
  bool promptsListChanged = false;
  bool resourcesListChanged = false;
  std::vector<std::string> resourceUris;  // notifications/resources/updated
};

// Receives one wire line per notification (or the graceful-closure result).
// Implementations must be safe to call from any thread that may publish.
using NotifSink = std::function<void(const std::string& line)>;

// Parses the spec's Notification Filter object from a subscriptions/listen
// params value; serializes a filter back (the acknowledged subset).
SubscriptionFilter parseSubscriptionFilter(const Json& params);
Json subscriptionFilterToJson(const SubscriptionFilter& filter);

// True when `method` has a server-side route (used by the HTTP transport for
// the 404-vs-405 distinction required by the spec).
bool isKnownMethod(std::string_view method);

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

  // The wire-form definition of a registered tool (Error Json if unknown).
  // Used by the HTTP transport to validate Mcp-Param-* mirrored headers.
  Json findToolDefinition(const std::string& name) const;

  // --- subscriptions ----------------------------------------------------------
  // processLine handles subscriptions/listen on the current channel (stdio:
  // acknowledged then later notifications as plain lines). Stream transports
  // instead call registerSubscription with their own sink after validating.

  // Registers a subscription and returns the acknowledged notification line
  // (the caller sends it before or after registering - server-side ordering
  // guarantees no notification precedes the acknowledgment).
  std::string registerSubscription(const RequestId& id, const SubscriptionFilter& filter,
                                   NotifSink sink);
  // Removes without a closure message (transport dropped).
  void removeSubscription(const std::string& subscriptionId);
  // Sends the graceful-closure result through the sink, then removes.
  void closeSubscription(const std::string& subscriptionId);

  // Fan-out helpers (thread-safe). Matched subscriptions receive the
  // notification stamped with their subscriptionId in _meta.
  void notifyListChanged(const std::string& feature);  // "tools"|"prompts"|"resources"
  void notifyResourceUpdated(const std::string& uri);

 private:
  Server() = default;
  Server(Server&& other) noexcept;  // mutex members are default-constructed

  struct Registry {
    std::vector<Json> definitions;             // wire form, served by *\/list
    std::map<std::string, Handler> handlers;   // key: name (or uri)
  };

  // handler dispatch, after meta/version/capability validation
  McpResult<Json> dispatchRequest(const JsonRpcRequest& req, const RequestMeta& meta,
                                  const std::function<bool()>& cancelled);

  McpResult<Json> handleDiscover(const RequestMeta& meta);
  McpResult<Json> handleInitialize(const JsonRpcRequest& req);
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
  // 2026-07-28 makes CacheableResult (ttlMs + cacheScope) REQUIRED on */list and
  // resources/read, so the conformant default is 0 (immediately stale) rather
  // than "absent"; strict modern clients (MCP SDK 2.0+) reject results without it.
  // listCache(-1) opts out explicitly (legacy-only servers / wire probing).
  long listTtlMs_ = 0;
  bool acceptLegacy_ = false;
  std::string listCacheScope_ = "public";
  std::string name_ = "mcp-cpp-server";
  std::string version_ = "0.1.0";
  Handler completionsHandler_;

  // request id (string form) -> cancel flag
  std::map<std::string, std::shared_ptr<std::atomic_bool>> cancellations_;

  struct Subscription {
    SubscriptionFilter filter;
    NotifSink sink;
    RequestId id = RequestId(int64_t(0));  // original listen request id
  };
  std::map<std::string, Subscription> subscriptions_;
  std::mutex subscriptionsMutex_;  // guards subscriptions_ (publish from any thread)
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

  // Convenience: a text tool whose inputSchema is {"type":"object"}; the
  // server validates arguments against it on tools/call (M4).
  Builder& simpleTool(std::string toolName, std::string description, Handler handler);

  // Registers the completion/complete handler and advertises the completions
  // capability. The handler receives the full params (ref + argument).
  Builder& completions(Handler handler);

  // CacheableResult hints (ttlMs/cacheScope) for */list and resources/read.
  // On by default with ttlMs 0 (required by 2026-07-28); pass a positive ttlMs
  // to let clients cache, or -1 to omit the fields entirely.
  Builder& listCache(long ttlMs, std::string cacheScope = "public");

  // Dual-era server: also accept LEGACY clients (initialize handshake, no
  // per-request _meta - e.g. Claude Code / VS Code on protocol revisions
  // <= 2025-11-25). Default off: the 2026-07-28 stateless gates stay strict.
  // When on:
  //   * `initialize` is served (legacy handshake) with an InitializeResult
  //   * requests without params._meta are served with default metadata
  //   * requests WITH an ill-formed _meta still fail -32602 (modern clients)
  Builder& acceptLegacyClients(bool on = true);

  std::shared_ptr<Server> build();

 private:
  Server server_;
};

}  // namespace server
}  // namespace mcp
