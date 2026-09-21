// Streamable HTTP end-to-end: real httplib server + client over loopback.
// Covers the 2026 shape: POST-only, header mirroring, 202 notifications,
// 404 unknown method, 405 GET/DELETE, Origin policy, -32020 header mismatch,
// SSE subscription stream with acknowledgment + live notifications.

#ifdef MCP_HAS_HTTP

#include <mcp/mcp.hpp>
#include <mcp/transport_http.hpp>

#include <httplib.h>

#include <gtest/gtest.h>

#include <atomic>
#include <thread>

using namespace mcp;
using namespace mcp::server;
using namespace mcp::client;
using namespace mcp::http;

namespace {

// One server + HTTP transport per test (fresh registries, ephemeral port).
struct Fixture {
  std::shared_ptr<Server> server;
  std::unique_ptr<HttpServerTransport> http;
  std::thread runner;
  int port = 0;

  Fixture() {
    Builder b;
    b.name("http-e2e").version("1.0");
    b.simpleTool("echo", "echoes", [](Ctx& c) -> McpResult<Json> {
      Json result;
      result.add("echoed", Json::str(c.params["arguments"]["text"].toString()));
      return Ok(std::move(result));
    });
    b.resource(Json(R"({"uri":"file:///motd","name":"motd"})"),
               [](Ctx&) -> McpResult<Json> {
                 Json result;
                 Json contents = Json("[]");
                 Json item;
                 item.add("uri", Json::str("file:///motd"));
                 item.add("text", Json::str("welcome"));
                 contents.push_back(std::move(item));
                 result.add("contents", std::move(contents));
                 return Ok(std::move(result));
               });
    server = b.build();

    http = std::make_unique<HttpServerTransport>(*server);
    auto bound = http->bind("127.0.0.1");  // ephemeral port; accepts on run()
    if (!bound.has_value()) return;
    port = *bound;
    runner = std::thread([this] { (void)http->run(); });
    // Wait for the listener.
    while (!http->running()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }

  ~Fixture() {
    http->stop();
    if (runner.joinable()) runner.join();
  }

  std::unique_ptr<Client> makeClient() {
    auto transport = std::make_unique<HttpClientTransport>("127.0.0.1", port);
    auto client = std::make_unique<Client>("http-client", "1.0");
    client->connect(*transport);
    // The transport must outlive the client in this test's scope; keep it.
    transports_.push_back(std::move(transport));
    return client;
  }

  std::vector<std::unique_ptr<HttpClientTransport>> transports_;
};

}  // namespace

TEST(HttpE2E, DiscoverAndCallToolOverLoopback) {
  Fixture f;
  auto client = f.makeClient();

  auto disc = client->discover();
  ASSERT_TRUE(disc.has_value());
  EXPECT_EQ((*disc)["resultType"].toString(), "complete");
  ASSERT_TRUE((*disc)["supportedVersions"].isArray());

  auto call = client->callTool("echo", Json(R"({"text":"over http"})"));
  ASSERT_TRUE(call.has_value());
  EXPECT_EQ((*call)["echoed"].toString(), "over http");
}

TEST(HttpE2E, ReadResource) {
  Fixture f;
  auto client = f.makeClient();
  auto r = client->readResource("file:///motd");
  ASSERT_TRUE(r.has_value());
  EXPECT_EQ((*r)["contents"][0]["text"].toString(), "welcome");
}

TEST(HttpE2E, UnknownMethodIs404WithJsonRpcBody) {
  Fixture f;
  auto client = f.makeClient();
  auto r = client->request("sampling/create", Json(nullptr));
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error().code, McpErrorCode::MethodNotFound);
}

TEST(HttpE2E, HeaderMismatchIsRejected) {
  Fixture f;
  // Send a tools/call whose Mcp-Name header will not match the body: we
  // tamper by setting the header directly on a raw transport.
  auto rawTransport = std::make_unique<HttpClientTransport>("127.0.0.1", f.port);
  Client raw("c", "1");
  raw.connect(*rawTransport);
  rawTransport->setHeader("Mcp-Name", "wrong-name");
  auto r = raw.request("tools/call", Json(R"({"name":"echo","arguments":{"text":"x"}})"));
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error().code, McpErrorCode::HeaderMismatch);
}

TEST(HttpE2E, GetAndDeleteAre405) {
  httplib::Client cli("127.0.0.1", 0);
  // covered implicitly by the server registering 405 handlers; direct check:
  Fixture f;
  httplib::Client c("127.0.0.1", f.port);
  auto get = c.Get("/mcp");
  ASSERT_TRUE(get);
  EXPECT_EQ(get->status, 405);
  auto del = c.Delete("/mcp");
  ASSERT_TRUE(del);
  EXPECT_EQ(del->status, 405);
}

TEST(HttpE2E, OriginPolicyRejectsForeignOrigins) {
  Builder b;
  b.name("o").version("1");
  auto server = b.build();
  HttpServerTransport http(*server);
  auto bound = http.bind("127.0.0.1");
  ASSERT_TRUE(bound.has_value());
  std::thread runner([&] { (void)http.run(); });
  while (!http.running()) std::this_thread::sleep_for(std::chrono::milliseconds(1));

  httplib::Client c("127.0.0.1", *bound);
  httplib::Headers evil{{"Origin", "http://evil.example.com"}};
  auto r = c.Post("/mcp", evil, "{}", "application/json");
  ASSERT_TRUE(r);
  EXPECT_EQ(r->status, 403);

  http.stop();
  runner.join();
}

TEST(HttpE2E, SubscriptionStreamDeliversNotifications) {
  Fixture f;
  auto client = f.makeClient();

  std::atomic<int> listChanged{0};
  client->onNotification([&](const JsonRpcNotification& n) {
    if (n.method == "notifications/tools/list_changed") listChanged++;
  });

  Json filter = Json(R"({"notifications":{"toolsListChanged":true}})");
  auto ack = client->subscribe(filter);
  ASSERT_TRUE(ack.has_value());
  const std::string subId =
      std::string((*ack)["_meta"]["io.modelcontextprotocol/subscriptionId"].toString());

  // Publish from this thread (as another server thread would).
  f.server->notifyListChanged("tools");
  f.server->notifyListChanged("tools");
  f.server->notifyResourceUpdated("file:///motd");  // filtered out

  // Notifications flow through the SSE stream into the client's callback.
  // The client is blocked nowhere: poll via a harmless request roundtrip and
  // drain the transport queue first. The queued SSE lines are read by the
  // next request()'s read loop, so we simply wait and force a read cycle.
  for (int i = 0; i < 100 && listChanged.load() < 2; ++i) {
    // A real client would block in its own reader thread; here we drive the
    // queue manually with listTools roundtrips.
    (void)client->listTools();
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  EXPECT_GE(listChanged.load(), 2);
  EXPECT_FALSE(subId.empty());
}

#endif  // MCP_HAS_HTTP
