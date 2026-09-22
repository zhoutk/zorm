#pragma once
// ---------------------------------------------------------------------------
// mcp/transport_http.hpp - Streamable HTTP binding (2026-07-28 shape).
//
// Compiled only when MCP_HAS_HTTP is defined (httplib available and
// MCP_DISABLE_HTTP off).
//
// Server side (HttpServerTransport):
//  - single POST endpoint; GET/DELETE answered 405 (no GET stream in 2026)
//  - header mirror validation: MCP-Protocol-Version, Mcp-Method, Mcp-Name;
//    mismatch/missing -> 400 + HeaderMismatch (-32020)
//  - unknown method -> 404 + -32601; notifications -> 202 no body
//  - requests answered application/json (SSE is used for subscriptions only)
//  - subscriptions/listen upgrades the response to an SSE stream that
//    receives notifications published via Server::notify* APIs
//
// Client side (HttpClientTransport):
//  - one POST per message with Accept: application/json, text/event-stream
//    and the mirrored request headers; x-mcp-header mirroring is done by
//    mcp::client::Client for tools/call (setMirroredHeaders)
//  - 202 yields no message; application/json / SSE bodies are pushed into
//    the read queue so the same ITransport read loop serves both bindings
// ---------------------------------------------------------------------------

#include <mcp/error.hpp>
#include <mcp/json.hpp>
#include <mcp/jsonrpc.hpp>
#include <mcp/server.hpp>
#include <mcp/transport.hpp>

#include <deque>
#include <functional>
#include <memory>
#include <string>

#if defined(MCP_HAS_HTTP)

namespace httplib {
class Server;
class Client;
}  // namespace httplib

namespace mcp {
namespace http {

// Implementation lives in transport_http.cpp; the class intentionally keeps
// httplib out of this header.
class HttpServerTransport {
 public:
  // Validates the Origin header. Empty string = no Origin header sent
  // (non-browser clients). Return false -> 403.
  using OriginValidator = std::function<bool(const std::string& origin)>;

  explicit HttpServerTransport(server::Server& server, std::string endpoint = "/mcp");
  ~HttpServerTransport();

  HttpServerTransport(const HttpServerTransport&) = delete;
  HttpServerTransport& operator=(const HttpServerTransport&) = delete;

  // Binds without accepting; returns the bound port (use port 0 for an
  // ephemeral port). run() then accepts on that socket.
  McpResult<int> bind(const std::string& host = "127.0.0.1");
  // Accepts until stop(); blocking (after bind()).
  McpResult<void> run();
  // Convenience for tests/tools: binds (port 0 allowed) and serves on a
  // background thread. Returns once the listener is accepting.
  McpResult<void> runAsync(const std::string& host = "127.0.0.1", int port = 8080);
  void stop();

  void setOriginValidator(OriginValidator validator);

  // True while the listener is accepting.
  bool running() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// ---- client side -------------------------------------------------------------

class HttpClientTransport final : public ITransport {
 public:
  HttpClientTransport(std::string host, int port, std::string endpoint = "/mcp");
  ~HttpClientTransport() override;

  // ITransport: write() performs the synchronous POST and queues response
  // message lines; read() pops them. Not thread-safe (single client loop).
  McpResult<std::optional<std::string>> read() override;
  McpResult<void> write(std::string_view message) override;
  McpResult<void> flush() override;

  // Adds/replaces a header sent with every POST (e.g. Authorization), or
  // per-request mirrored headers (Mcp-Param-*). Empty value removes.
  void setHeader(std::string name, std::string value) override;

  // Terminates all open subscription streams (background SSE connections)
  // and waits for their threads to finish. Safe to call from any thread;
  // idempotent. Called automatically by the destructor. After this returns,
  // no further notification lines will be queued.
  void closeSubscriptions() override;

 private:
  McpResult<void> postOne(std::string_view message);
  void queueSseBody(const std::string& body);

  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace http
}  // namespace mcp

#endif  // MCP_HAS_HTTP
