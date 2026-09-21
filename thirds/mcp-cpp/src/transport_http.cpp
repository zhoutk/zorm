#include "base64.h"
#include "header_params.h"

#include <mcp/transport_http.hpp>

#if defined(MCP_HAS_HTTP)

#include <httplib.h>

#include <atomic>
#include <chrono>
#include <cctype>
#include <condition_variable>
#include <map>
#include <deque>
#include <thread>
#include <utility>

namespace mcp {
namespace http {
namespace {

constexpr const char* kHeaderProtocolVersion = "MCP-Protocol-Version";
constexpr const char* kHeaderMethod = "Mcp-Method";
constexpr const char* kHeaderName = "Mcp-Name";

bool methodNeedsNameHeader(std::string_view method) {
  return method == "tools/call" || method == "resources/read" || method == "prompts/get";
}

std::string errorBody(const std::optional<RequestId>& id, int32_t code, std::string message) {
  JsonRpcErrorValue v;
  v.code = code;
  v.message = std::move(message);
  return encodeErrorResponse(id, v);
}

std::optional<RequestId> idOf(const ParsedMessage& parsed) {
  if (const auto* r = std::get_if<JsonRpcRequest>(&parsed)) return r->id;
  return std::nullopt;
}

std::string idToString(const RequestId& id) {
  if (const auto* i = std::get_if<int64_t>(&id)) return std::to_string(*i);
  return std::get<std::string>(id);
}

// Thread-safe line queue shared between Server fan-out threads and the SSE
// writer loop of one subscription.
struct Pump {
  std::mutex mu;
  std::condition_variable cv;
  std::deque<std::string> lines;
  bool closed = false;

  void push(std::string line) {
    {
      std::lock_guard<std::mutex> lock(mu);
      lines.push_back(std::move(line));
    }
    cv.notify_one();
  }
  void close() {
    {
      std::lock_guard<std::mutex> lock(mu);
      closed = true;
    }
    cv.notify_all();
  }
  // Pops one line; blocks up to timeoutMs. Returns: 1 = line, 0 = timeout,
  // -1 = closed and drained.
  int waitPop(std::string& out, int timeoutMs) {
    std::unique_lock<std::mutex> lock(mu);
    if (!cv.wait_for(lock, std::chrono::milliseconds(timeoutMs), [&] {
          return closed || !lines.empty();
        })) {
      return 0;
    }
    if (!lines.empty()) {
      out = std::move(lines.front());
      lines.pop_front();
      return 1;
    }
    return -1;  // closed and drained
  }
};

}  // namespace

// ---- server transport ----------------------------------------------------------

struct HttpServerTransport::Impl {
  server::Server& server;
  httplib::Server svr;
  std::string endpoint;
  OriginValidator originValidator;
  std::atomic_bool running{false};
  std::thread thread;
  std::mutex pumpMutex;                        // guards pumps
  std::vector<std::shared_ptr<Pump>> pumps;    // active subscription streams

  Impl(server::Server& s, std::string ep) : server(s), endpoint(std::move(ep)) {
    originValidator = [](const std::string& origin) {
      // Default policy: no Origin header (non-browser clients) and explicit
      // localhost origins pass; everything else is rejected (DNS-rebinding
      // protection from the spec's Security & Endpoint section).
      if (origin.empty()) return true;
      return origin.rfind("http://localhost", 0) == 0 ||
             origin.rfind("http://127.0.0.1", 0) == 0;
    };
  }

  // Header mirror validation. Returns {httpStatus, body}; status 0 = pass.
  std::pair<int, std::string> validateHeaders(const httplib::Request& req,
                                              const ParsedMessage& parsed) const {
    const JsonRpcRequest* request = std::get_if<JsonRpcRequest>(&parsed);
    const JsonRpcNotification* notification = std::get_if<JsonRpcNotification>(&parsed);
    if (request == nullptr && notification == nullptr) {
      return {400, errorBody(std::nullopt, static_cast<int32_t>(McpErrorCode::InvalidRequest),
                             "Invalid request: expected a JSON-RPC request or notification")};
    }
    const std::string method = request ? request->method : notification->method;
    const Json& params = request ? request->params : notification->params;
    const std::optional<RequestId> id = idOf(parsed);

    const std::string headerVersion = req.get_header_value(kHeaderProtocolVersion);
    if (headerVersion.empty()) {
      return {400, errorBody(id, static_cast<int32_t>(McpErrorCode::HeaderMismatch),
                             "Header mismatch: MCP-Protocol-Version header is missing")};
    }
    if (request) {
      const Json meta = member(params, "_meta");
      const Json bodyVersion = member(meta, kMetaProtocolVersion);
      if (bodyVersion.isString() && bodyVersion.toString() != headerVersion) {
        return {400, errorBody(id, static_cast<int32_t>(McpErrorCode::HeaderMismatch),
                               "Header mismatch: MCP-Protocol-Version does not match body _meta")};
      }
    }

    const std::string headerMethod = req.get_header_value(kHeaderMethod);
    if (headerMethod.empty()) {
      return {400, errorBody(id, static_cast<int32_t>(McpErrorCode::HeaderMismatch),
                             "Header mismatch: Mcp-Method header is missing")};
    }
    if (headerMethod != method) {
      return {400, errorBody(id, static_cast<int32_t>(McpErrorCode::HeaderMismatch),
                             "Header mismatch: Mcp-Method header does not match body method")};
    }

    if (methodNeedsNameHeader(method)) {
      const std::string headerName = req.get_header_value(kHeaderName);
      const char* sourceKey = (method == "tools/call" || method == "prompts/get") ? "name" : "uri";
      const Json name = member(params, sourceKey);
      if (!name.isString() || headerName.empty()) {
        return {400, errorBody(id, static_cast<int32_t>(McpErrorCode::HeaderMismatch),
                               "Header mismatch: Mcp-Name header is missing or malformed")};
      }
      std::string decoded;
      if (!detail::decodeHeaderValue(headerName, decoded) || decoded != name.toString()) {
        return {400, errorBody(id, static_cast<int32_t>(McpErrorCode::HeaderMismatch),
                               "Header mismatch: Mcp-Name header does not match body value")};
      }
    }

    // Mcp-Param-* mirrored tool parameters: every annotated parameter with a
    // value in the body must appear as a header with the matching value, and
    // every mirrored header must correspond to an annotated parameter.
    if (request != nullptr && request->method == "tools/call") {
      const Json toolDef = server.findToolDefinition(
          std::string(request->params["name"].toString()));
      const Json inputSchema = member(toolDef, "inputSchema");
      if (inputSchema.isObject()) {
        std::vector<std::pair<std::string, std::string>> expected;
        detail::collectHeaderParams(inputSchema, request->params["arguments"], {}, expected);

        // index received Mcp-Param-* headers (case-insensitive names)
        std::map<std::string, std::string> received;  // upper(name) -> (name,value)
        for (const auto& field : req.headers) {
          std::string upper;
          for (const char ch : field.first) {
            upper.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(ch))));
          }
          if (upper.rfind("MCP-PARAM-", 0) == 0) received[upper] = field.second;
        }

        for (const auto& [paramName, rawValue] : expected) {
          std::string upper = "MCP-PARAM-";
          for (const char ch : paramName) {
            upper.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(ch))));
          }
          auto it = received.find(upper);
          if (it == received.end()) {
            return {400, errorBody(id, static_cast<int32_t>(McpErrorCode::HeaderMismatch),
                                   "Header mismatch: Mcp-Param-" + paramName +
                                       " header is missing")};
          }
          std::string decoded;
          if (!detail::decodeHeaderValue(it->second, decoded) || decoded != rawValue) {
            return {400, errorBody(id, static_cast<int32_t>(McpErrorCode::HeaderMismatch),
                                   "Header mismatch: Mcp-Param-" + paramName +
                                       " does not match body value")};
          }
          received.erase(upper);
        }
        if (!received.empty()) {
          return {400, errorBody(id, static_cast<int32_t>(McpErrorCode::HeaderMismatch),
                                 "Header mismatch: unrecognized Mcp-Param-* header")};
        }
      }
    }
    return {0, ""};
  }

  void registerHandlers() {
    svr.Post(endpoint, [this](const httplib::Request& req, httplib::Response& res) {
      handlePost(req, res);
    });
    // 2026 revision: no GET stream, no session DELETE.
    svr.Get(endpoint, [](const httplib::Request&, httplib::Response& res) {
      res.status = 405;
    });
    svr.Delete(endpoint, [](const httplib::Request&, httplib::Response& res) {
      res.status = 405;
    });
  }

  void handlePost(const httplib::Request& req, httplib::Response& res) {
    // 1. Origin validation.
    const std::string origin = req.get_header_value("Origin");
    if (!originValidator(origin)) {
      res.status = 403;
      return;
    }

    // 2. Parse the body.
    const ParsedMessage parsed = parseMessage(req.body);
    const bool isNotification = std::holds_alternative<JsonRpcNotification>(parsed);

    // 3. Header mirror validation.
    const auto [status, body] = validateHeaders(req, parsed);
    if (status != 0) {
      res.status = status;
      res.set_content(body, "application/json");
      return;
    }

    const JsonRpcRequest* request = std::get_if<JsonRpcRequest>(&parsed);
    const JsonRpcNotification* notification = std::get_if<JsonRpcNotification>(&parsed);
    const std::string method = request ? request->method : notification->method;

    // 4. Unknown method -> 404 + -32601 (distinguishes modern servers from
    // legacy HTTP+SSE hosts, which 404 with an empty body).
    if (request != nullptr && !server::isKnownMethod(method)) {
      res.status = 404;
      res.set_content(
          errorBody(request->id, static_cast<int32_t>(McpErrorCode::MethodNotFound),
                    "Method not found: " + method),
          "application/json");
      return;
    }

    // 5. subscriptions/listen -> SSE stream (acknowledged + live notifications).
    if (request != nullptr && method == "subscriptions/listen") {
      startSubscriptionStream(*request, res);
      return;
    }

    // 6. Everything else: dispatch through the shared server logic into a
    // capture sink, then answer.
    std::string responseLine;
    LineCapture sink(responseLine);
    server.processLine(req.body, sink);

    if (isNotification) {
      // Accepted notification: 202 with no body (any server error response to
      // a notification - malformed params - still flows through the capture).
      if (responseLine.empty()) {
        res.status = 202;
      } else {
        res.status = 400;  // server rejected the notification (invalid params)
        res.set_content(responseLine, "application/json");
      }
      return;
    }

    res.status = 200;
    res.set_content(responseLine, "application/json");
  }

  // Collects the single line the server writes for one request.
  struct LineCapture final : ITransport {
    explicit LineCapture(std::string& out) : out_(out) {}
    McpResult<std::optional<std::string>> read() override {
      return Ok(std::optional<std::string>(std::nullopt));
    }
    McpResult<void> write(std::string_view message) override {
      out_.assign(message);
      return Ok();
    }
    McpResult<void> flush() override { return Ok(); }

   private:
    std::string& out_;
  };

  void startSubscriptionStream(const JsonRpcRequest& request, httplib::Response& res) {
    auto pump = std::make_shared<Pump>();
    const server::SubscriptionFilter filter = server::parseSubscriptionFilter(request.params);

    res.status = 200;
    res.set_header("Cache-Control", "no-cache");
    res.set_header("X-Accel-Buffering", "no");
    res.set_chunked_content_provider(
        "text/event-stream",
        [this, pump, request, filter](size_t /*offset*/, httplib::DataSink& sink) -> bool {
          // Acknowledgment MUST be the first message on the stream.
          {
            Json ackParams;
            Json ackMeta;
            ackMeta.add(std::string(kMetaSubscriptionId), encodeRequestId(request.id));
            ackParams.add("_meta", std::move(ackMeta));
            ackParams.add("notifications", server::subscriptionFilterToJson(filter));
            std::string line = "data: ";
            line += encodeNotification("notifications/subscriptions/acknowledged", ackParams);
            line += "\n\n";
            if (!sink.write(line.data(), line.size())) return false;
          }

          {
            std::lock_guard<std::mutex> lock(pumpMutex);
            pumps.push_back(pump);
          }
          server.registerSubscription(request.id, filter,
                                      [pump](const std::string& wireLine) {
                                        pump->push(wireLine);
                                      });

          // Pump until the client disconnects or the server is stopped.
          while (running.load()) {
            std::string line;
            const int got = pump->waitPop(line, 15000);  // keep-alive cadence
            if (got == 1) {
              std::string event = "data: ";
              event += line;
              event += "\n\n";
              if (!sink.write(event.data(), event.size())) break;
            } else if (got == 0) {
              static const char kKeepAlive[] = ":\n\n";
              if (!sink.write(kKeepAlive, 3)) break;  // SSE comment keep-alive
            } else {
              break;
            }
          }
          server.removeSubscription(idToString(request.id));
          {
            std::lock_guard<std::mutex> lock(pumpMutex);
            for (auto it = pumps.begin(); it != pumps.end(); ++it) {
              if (*it == pump) { pumps.erase(it); break; }
            }
          }
          sink.done();
          return true;
        });
  }
};

HttpServerTransport::HttpServerTransport(server::Server& server, std::string endpoint)
    : impl_(std::make_unique<Impl>(server, std::move(endpoint))) {
  impl_->registerHandlers();
}

HttpServerTransport::~HttpServerTransport() { stop(); }

McpResult<int> HttpServerTransport::bind(const std::string& host) {
  const int port = impl_->svr.bind_to_any_port(host);
  if (port <= 0) return Err(McpErrorCode::InternalError, "http bind failed");
  return Ok(port);
}

McpResult<void> HttpServerTransport::run() {
  impl_->running = true;
  const bool ok = impl_->svr.listen_after_bind();
  impl_->running = false;
  if (!ok) return Err(McpErrorCode::InternalError, "http server stopped");
  return Ok();
}

McpResult<void> HttpServerTransport::runAsync(const std::string& host, int port) {
  if (port == 0) {
    auto bound = bind(host);
    if (!bound.has_value()) return Err(bound.error());
    impl_->thread = std::thread([this] { (void)run(); });
    return Ok();
  }
  impl_->running = true;
  impl_->thread = std::thread([this, host, port] {
    if (!impl_->svr.listen(host, port)) impl_->running = false;
  });
  // Wait until the listener is accepting (or failed).
  for (int i = 0; i < 5000 && !impl_->svr.is_running(); ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  const bool accepting = impl_->svr.is_running();
  if (!accepting) return Err(McpErrorCode::InternalError, "http listen failed");
  return Ok();
}

void HttpServerTransport::stop() {
  // Signal SSE providers BEFORE svr.stop(): httplib's stop() joins worker
  // threads, and is_running() only clears after that join - the providers
  // would deadlock waiting on it.
  impl_->running = false;
  impl_->svr.stop();
  if (impl_->thread.joinable()) impl_->thread.join();
}

bool HttpServerTransport::running() const { return impl_->svr.is_running(); }

void HttpServerTransport::setOriginValidator(OriginValidator validator) {
  impl_->originValidator = std::move(validator);
}

// ---- client transport -----------------------------------------------------------

struct HttpClientTransport::Impl {
  std::string endpoint;
  httplib::Client cli;
  std::map<std::string, std::string, std::less<>> headers;
  std::mutex queueMutex;          // guards queue
  std::condition_variable queueCv;
  std::deque<std::string> queue;  // completed message lines for read()

  void pushQueue(std::string line) {
    {
      std::lock_guard<std::mutex> lock(queueMutex);
      queue.push_back(std::move(line));
    }
    queueCv.notify_one();
  }

  // Fresh connection per SSE stream: keeps the shared cli single-threaded.
  std::unique_ptr<httplib::Client> streamClient;

  Impl(std::string host, int port, std::string ep)
      : endpoint(std::move(ep)), cli(std::move(host), port) {}
};

HttpClientTransport::HttpClientTransport(std::string host, int port, std::string endpoint)
    : impl_(std::make_unique<Impl>(std::move(host), port, std::move(endpoint))) {
  impl_->cli.set_connection_timeout(5);
  impl_->cli.set_read_timeout(30, 0);  // long enough for SSE test streams
}

HttpClientTransport::~HttpClientTransport() = default;

void HttpClientTransport::setHeader(std::string name, std::string value) {
  if (value.empty()) {
    impl_->headers.erase(name);
  } else {
    impl_->headers[name] = std::move(value);
  }
}

McpResult<std::optional<std::string>> HttpClientTransport::read() {
  // Blocking read: SSE lines arrive asynchronously on the background stream
  // thread, so a momentarily empty queue is not EOF.
  std::unique_lock<std::mutex> lock(impl_->queueMutex);
  if (!impl_->queueCv.wait_for(lock, std::chrono::seconds(30),
                               [&] { return !impl_->queue.empty(); })) {
    return Ok(std::optional<std::string>(std::nullopt));  // idle timeout
  }
  std::string line = std::move(impl_->queue.front());
  impl_->queue.pop_front();
  return Ok(std::optional<std::string>(std::move(line)));
}

McpResult<void> HttpClientTransport::write(std::string_view message) {
  return postOne(message);
}

McpResult<void> HttpClientTransport::flush() { return Ok(); }

namespace {


}  // namespace

McpResult<void> HttpClientTransport::postOne(std::string_view message) {
  // Derive the mirrored headers from the outgoing message itself.
  const ParsedMessage parsed = parseMessage(message);
  const JsonRpcRequest* request = std::get_if<JsonRpcRequest>(&parsed);
  const JsonRpcNotification* notification = std::get_if<JsonRpcNotification>(&parsed);
  if (request == nullptr && notification == nullptr) {
    return Err(McpErrorCode::InvalidRequest, "http transport only sends requests/notifications");
  }
  const std::string method = request ? request->method : notification->method;
  const Json& params = request ? request->params : notification->params;

  httplib::Headers headers;
  headers.emplace("Accept", "application/json, text/event-stream");
  const Json meta = member(params, "_meta");
  const Json version = member(meta, kMetaProtocolVersion);
  if (version.isString()) headers.emplace(kHeaderProtocolVersion, std::string(version.toString()));
  headers.emplace(kHeaderMethod, method);
  if (methodNeedsNameHeader(method)) {
    const char* key = (method == "tools/call" || method == "prompts/get") ? "name" : "uri";
    const Json name = member(params, key);
    if (name.isString()) {
      headers.emplace(kHeaderName, detail::encodeHeaderValue(name.toString()));
    }
  }
  for (const auto& [k, v] : impl_->headers) {
    headers.erase(k);  // override the body-derived mirror (e.g. tamper tests)
    headers.emplace(k, v);
  }

  // subscriptions/listen: the response is a long-lived SSE stream. Serve it
  // on a background thread with a dedicated connection so the client loop
  // stays responsive; the acknowledgment is queued like any other message.
  if (request != nullptr && request->method == "subscriptions/listen") {
    std::string body(message);
    auto headersCopy = headers;
    auto stream = std::make_unique<httplib::Client>(impl_->cli.host(), impl_->cli.port());
    stream->set_connection_timeout(5);
    std::thread([this, stream = std::move(stream), body = std::move(body),
                 headersCopy = std::move(headersCopy)]() mutable {
      std::string acc;
      auto receiver = [this, &acc](const char* data, size_t len) -> bool {
        acc.append(data, len);
        std::deque<std::string> events;
        {
          static const std::string kEventEnd = "\n\n";
          size_t pos;
          while ((pos = acc.find(kEventEnd)) != std::string::npos) {
            const std::string event = acc.substr(0, pos);
            acc.erase(0, pos + kEventEnd.size());
            if (event.rfind("data: ", 0) == 0) events.push_back(event.substr(6));
          }
        }
        for (auto& e : events) impl_->pushQueue(std::move(e));
        return true;
      };
      // Blocks until the server closes the stream (graceful closure, stop,
      // or transport drop) - exactly the subscription lifetime.
      (void)stream->Post(impl_->endpoint.c_str(), headersCopy, body, "application/json",
                         receiver);
      {
        std::string tail = std::move(acc);
        std::deque<std::string> events;
        static const std::string kEventEnd = "\n\n";
        size_t pos;
        while ((pos = tail.find(kEventEnd)) != std::string::npos) {
          const std::string event = tail.substr(0, pos);
          tail.erase(0, pos + kEventEnd.size());
          if (event.rfind("data: ", 0) == 0) events.push_back(event.substr(6));
        }
        for (auto& e : events) impl_->pushQueue(std::move(e));
      }
    }).detach();
    return Ok();
  }

  auto result = impl_->cli.Post(impl_->endpoint.c_str(), headers,
                                std::string(message), "application/json");
  if (!result) {
    return Err(McpErrorCode::InternalError,
               "http POST failed: " + httplib::to_string(result.error()));
  }
  auto& res = *result;

  if (res.status == 202) return Ok();  // accepted notification, no body

  const std::string& contentType = res.get_header_value("Content-Type");
  if (contentType.rfind("text/event-stream", 0) == 0) {
    queueSseBody(res.body);
    return Ok();
  }
  if (!res.body.empty()) {
    // application/json (or an error body) - one message line.
    impl_->pushQueue(res.body);
  } else if (res.status >= 400) {
    return Err(McpErrorCode::InternalError,
               "http POST returned status " + std::to_string(res.status));
  }
  return Ok();
}

void HttpClientTransport::queueSseBody(const std::string& body) {
  // Split SSE events: each `data: <json>` line is one queued message.
  size_t pos = 0;
  while (pos < body.size()) {
    const size_t eol = body.find('\n', pos);
    const std::string_view line = std::string_view(body).substr(
        pos, eol == std::string::npos ? body.size() - pos : eol - pos);
    if (line.rfind("data: ", 0) == 0) {
      std::lock_guard<std::mutex> lock(impl_->queueMutex);
      impl_->queue.push_back(std::string(line.substr(6)));
    }
    if (eol == std::string::npos) break;
    pos = eol + 1;
  }
}

}  // namespace http
}  // namespace mcp

#endif  // MCP_HAS_HTTP
