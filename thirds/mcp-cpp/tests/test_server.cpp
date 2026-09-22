// Server runtime: stateless dispatch, meta/version/capability gates,
// tools/resources/prompts registries, cancellation, pagination.

#include "MemTransport.h"

#include <mcp/mcp.hpp>

#include <gtest/gtest.h>

#include <string>
#include <variant>

using namespace mcp;
using namespace mcp::server;

namespace {

struct Out {
  ParsedMessage parsed;
  const JsonRpcResult* result = nullptr;
  const JsonRpcErrorResponse* error = nullptr;
  const JsonRpcNotification* notification = nullptr;
};

// Drives one request through the server and decodes the (single) response.
Out serveOnce(const std::shared_ptr<Server>& server, const std::string& requestLine) {
  MemTransport t({requestLine});
  server->processLine(requestLine, t);
  EXPECT_EQ(t.output_.size(), 1u);
  Out o;
  if (t.output_.empty()) return o;
  o.parsed = parseMessage(t.output_[0]);
  o.result = std::get_if<JsonRpcResult>(&o.parsed);
  o.error = std::get_if<JsonRpcErrorResponse>(&o.parsed);
  o.notification = std::get_if<JsonRpcNotification>(&o.parsed);
  return o;
}

// Builds a modern request line with the required _meta.
std::string makeRequest(const std::string& id, const std::string& method,
                        const Json& extraParams, const std::string& version = "2026-07-28") {
  Json params = encodeRequestMeta(version, Json(R"({})"), std::nullopt);
  if (extraParams.isObject()) {
    for (const auto& item : extraParams.getAllKeys().toVector()) {
      params.add(item.toString(), extraParams[item.toString()]);
    }
  }
  return encodeRequest(RequestId(id), method, params);
}

std::shared_ptr<Server> makeToolServer() {
  Builder b;
  b.name("test").version("1.0");
  b.simpleTool("echo", "echoes", [](Ctx& c) -> McpResult<Json> {
    Json result;
    result.add("echoed", Json::str(c.params["arguments"]["text"].toString()));
    return Ok(std::move(result));
  });
  b.simpleTool("fail", "always errors", [](Ctx&) -> McpResult<Json> {
    return Err(McpErrorCode::InvalidParams, "intentional failure");
  });
  b.simpleTool("longrun", "checks cancellation", [](Ctx& c) -> McpResult<Json> {
    if (c.isCancelled()) {
      return Err(McpErrorCode::InternalError, "cancelled");
    }
    Json result;
    result.add("done", Json(true));
    return Ok(std::move(result));
  });
  return b.build();
}

}  // namespace

// ---- discover --------------------------------------------------------------------

TEST(ServerDiscover, ReturnsVersionsCapabilitiesAndCacheHints) {
  Builder b;
  b.name("disc").version("9.9").instructions("howto").discoverTtlMs(5000)
      .supports(ProtocolVersion::V2025_11_25).supports(ProtocolVersion::Latest);
  auto server = b.build();

  Out o = serveOnce(server, makeRequest("d1", "server/discover", Json(nullptr)));
  ASSERT_NE(o.result, nullptr);
  const Json& r = o.result->result;
  EXPECT_EQ(r["resultType"].toString(), "complete");

  const Json versions = r["supportedVersions"];
  ASSERT_TRUE(versions.isArray());
  ASSERT_EQ(versions.size(), 2);
  EXPECT_EQ(versions[0].toString(), "2025-11-25");  // enum order, not call order
  EXPECT_EQ(versions[1].toString(), "2026-07-28");

  EXPECT_TRUE(r["capabilities"].isObject());
  EXPECT_EQ(r["ttlMs"].toInt64(), 5000);
  EXPECT_EQ(r["cacheScope"].toString(), "public");
  EXPECT_EQ(r["instructions"].toString(), "howto");
  EXPECT_EQ(r["_meta"]["io.modelcontextprotocol/serverInfo"]["name"].toString(), "disc");
}

// ---- meta / version gates ----------------------------------------------------------

TEST(ServerGates, MissingMetaIsInvalidParams) {
  auto server = makeToolServer();
  const std::string line =
      encodeRequest(RequestId(std::string("m1")), "tools/list", Json(nullptr));
  Out o = serveOnce(server, line);
  ASSERT_NE(o.error, nullptr);
  EXPECT_EQ(o.error->error.code, -32602);
}

TEST(ServerGates, UnsupportedVersionListsSupported) {
  auto server = makeToolServer();
  Out o = serveOnce(server, makeRequest("v1", "tools/list", Json(nullptr), "2025-03-26"));
  ASSERT_NE(o.error, nullptr);
  EXPECT_EQ(o.error->error.code, -32022);
  const Json supported = o.error->error.data["supported"];
  ASSERT_TRUE(supported.isArray());
  EXPECT_EQ(supported[0].toString(), "2026-07-28");
  EXPECT_EQ(o.error->error.data["requested"].toString(), "2025-03-26");
}

TEST(ServerGates, UnsupportedVersionAcceptedWhenConfigured) {
  Builder b;
  b.supports(ProtocolVersion::V2025_11_25).supports(ProtocolVersion::Latest);
  b.simpleTool("t", "", [](Ctx&) -> McpResult<Json> { return Ok(Json(R"({"ok":true})")); });
  auto server = b.build();

  Out o = serveOnce(server, makeRequest("v2", "tools/call",
                                        Json(R"({"name":"t"})"), "2025-11-25"));
  ASSERT_NE(o.result, nullptr);
  EXPECT_EQ(o.result->result["ok"].isTrue(), true);
}

TEST(ServerGates, UnknownMethodIsMethodNotFound) {
  auto server = makeToolServer();
  Out o = serveOnce(server, makeRequest("u1", "sampling/create", Json(nullptr)));
  ASSERT_NE(o.error, nullptr);
  EXPECT_EQ(o.error->error.code, -32601);
}

TEST(ServerGates, UnknownNotificationIsSilentlyIgnored) {
  auto server = makeToolServer();
  MemTransport t({});
  server->processLine(encodeNotification("notifications/whatever", Json(nullptr)), t);
  EXPECT_TRUE(t.output_.empty());
}

TEST(ServerGates, InvalidMessageGetsParseErrorWithoutId) {
  auto server = makeToolServer();
  MemTransport t({});
  server->processLine("not json", t);
  ASSERT_EQ(t.output_.size(), 1u);
  ParsedMessage parsed = parseMessage(t.output_[0]);
  const auto* err = std::get_if<JsonRpcErrorResponse>(&parsed);
  ASSERT_NE(err, nullptr);
  EXPECT_EQ(err->error.code, -32700);
  EXPECT_FALSE(err->id.has_value());
}

// ---- tools --------------------------------------------------------------------------

TEST(ServerTools, ListAndCall) {
  auto server = makeToolServer();
  Out list = serveOnce(server, makeRequest("l1", "tools/list", Json(nullptr)));
  ASSERT_NE(list.result, nullptr);
  const Json tools = list.result->result["tools"];
  ASSERT_TRUE(tools.isArray());
  EXPECT_EQ(tools.size(), 3);  // echo, fail, longrun
  EXPECT_EQ(list.result->result["resultType"].toString(), "complete");

  Out call = serveOnce(server, makeRequest(
                                 "c1", "tools/call",
                                 Json(R"({"name":"echo","arguments":{"text":"hi"}})")));
  ASSERT_NE(call.result, nullptr);
  EXPECT_EQ(call.result->result["echoed"].toString(), "hi");
}

TEST(ServerTools, UnknownToolIsInvalidParams) {
  auto server = makeToolServer();
  Out o = serveOnce(server, makeRequest("c2", "tools/call", Json(R"({"name":"nope"})")));
  ASSERT_NE(o.error, nullptr);
  EXPECT_EQ(o.error->error.code, -32602);
}

TEST(ServerTools, CallWithoutAdvertisedCapabilityIsMethodNotFound) {
  // No tools registered -> tools capability never advertised.
  Builder b;
  auto server = b.build();
  Out o = serveOnce(server, makeRequest("c3", "tools/call", Json(R"({"name":"t"})")));
  ASSERT_NE(o.error, nullptr);
  EXPECT_EQ(o.error->error.code, -32601);
}

TEST(ServerTools, HandlerResultGetsResultTypeInjected) {
  auto server = makeToolServer();
  Out call = serveOnce(server, makeRequest("c4", "tools/call",
                                           Json(R"({"name":"echo","arguments":{"text":"x"}})")));
  ASSERT_NE(call.result, nullptr);
  EXPECT_EQ(call.result->result["resultType"].toString(), "complete");
}

TEST(ServerTools, HandlerErrorBecomesErrorResponse) {
  auto server = makeToolServer();
  Out call = serveOnce(server, makeRequest("c5", "tools/call", Json(R"({"name":"fail"})")));
  ASSERT_NE(call.error, nullptr);
  EXPECT_EQ(call.error->error.code, -32602);  // handler's own InvalidParams
  EXPECT_EQ(call.error->error.message, "intentional failure");
}

// ---- resources / prompts -------------------------------------------------------------

TEST(ServerRegistrys, ResourcesAndPrompts) {
  Builder b;
  b.resource(Json(R"({"uri":"file:///etc/motd","name":"motd"})"),
             [](Ctx&) -> McpResult<Json> {
               Json result;
               Json contents = Json("[]");
               Json item;
               item.add("uri", Json::str("file:///etc/motd"));
               item.add("text", Json::str("hello"));
               contents.push_back(std::move(item));
               result.add("contents", std::move(contents));
               return Ok(std::move(result));
             });
  b.prompt(Json(R"({"name":"greet","description":"greeting"})"),
           [](Ctx&) -> McpResult<Json> {
             Json result;
             Json messages = Json("[]");
             Json msg;
             msg.add("role", Json::str("user"));
             Json content;
             content.add("type", Json::str("text"));
             content.add("text", Json::str("hi there"));
             msg.add("content", std::move(content));
             messages.push_back(std::move(msg));
             result.add("messages", std::move(messages));
             return Ok(std::move(result));
           });
  auto server = b.build();

  Out rl = serveOnce(server, makeRequest("r1", "resources/list", Json(nullptr)));
  ASSERT_NE(rl.result, nullptr);
  EXPECT_EQ(rl.result->result["resources"].size(), 1);

  Out rr = serveOnce(server, makeRequest("r2", "resources/read",
                                         Json(R"({"uri":"file:///etc/motd"})")));
  ASSERT_NE(rr.result, nullptr);
  EXPECT_EQ(rr.result->result["contents"][0]["text"].toString(), "hello");

  Out pl = serveOnce(server, makeRequest("p1", "prompts/list", Json(nullptr)));
  ASSERT_NE(pl.result, nullptr);
  EXPECT_EQ(pl.result->result["prompts"].size(), 1);

  Out pg = serveOnce(server, makeRequest("p2", "prompts/get",
                                         Json(R"({"name":"greet"})")));
  ASSERT_NE(pg.result, nullptr);
  EXPECT_EQ(pg.result->result["messages"][0]["content"]["text"].toString(), "hi there");

  // Unknown resource/prompt are InvalidParams.
  Out bad = serveOnce(server, makeRequest("r3", "resources/read",
                                          Json(R"({"uri":"file:///nope"})")));
  ASSERT_NE(bad.error, nullptr);
  EXPECT_EQ(bad.error->error.code, -32602);
}

// ---- cancellation ----------------------------------------------------------------------

TEST(ServerCancellation, CancelledBeforeRequestIsObserved) {
  auto server = makeToolServer();
  // notifications/cancelled names id "cancel-1" before the matching request.
  Json cancelParams;
  cancelParams.add("requestId", Json::str("cancel-1"));
  MemTransport t({});
  server->processLine(encodeNotification("notifications/cancelled", cancelParams), t);
  EXPECT_TRUE(t.output_.empty());  // never answered

  Out o = serveOnce(server, makeRequest("cancel-1", "tools/call",
                                        Json(R"({"name":"longrun"})")));
  ASSERT_NE(o.error, nullptr);
  EXPECT_EQ(o.error->error.message, "cancelled");
}

// ---- pagination ---------------------------------------------------------------------------

TEST(ServerPagination, CursorPages) {
  Builder b;
  b.pageSize(1);
  for (const char* n : {"t1", "t2", "t3"}) {
    b.simpleTool(n, "", [](Ctx&) -> McpResult<Json> { return Ok(Json(R"({})")); });
  }
  auto server = b.build();

  Out p1 = serveOnce(server, makeRequest("pg1", "tools/list", Json(nullptr)));
  ASSERT_NE(p1.result, nullptr);
  EXPECT_EQ(p1.result->result["tools"].size(), 1);
  EXPECT_EQ(p1.result->result["nextCursor"].toString(), "1");

  Out p2 = serveOnce(server, makeRequest("pg2", "tools/list",
                                         Json(R"({"cursor":"1"})")));
  ASSERT_NE(p2.result, nullptr);
  EXPECT_EQ(p2.result->result["tools"][0]["name"].toString(), "t2");
  EXPECT_EQ(p2.result->result["nextCursor"].toString(), "2");

  Out p3 = serveOnce(server, makeRequest("pg3", "tools/list",
                                         Json(R"({"cursor":"2"})")));
  ASSERT_NE(p3.result, nullptr);
  EXPECT_EQ(p3.result->result["tools"][0]["name"].toString(), "t3");
  EXPECT_TRUE(p3.result->result["nextCursor"].isError());  // absent

  Out bad = serveOnce(server, makeRequest("pg4", "tools/list",
                                          Json(R"({"cursor":"x"})")));
  ASSERT_NE(bad.error, nullptr);
  EXPECT_EQ(bad.error->error.code, -32602);
}

// ---- run loop ---------------------------------------------------------------------------------

TEST(ServerRun, ProcessesUntilEof) {
  auto server = makeToolServer();
  std::vector<std::string> in = {
      makeRequest("1", "server/discover", Json(nullptr)),
      encodeNotification("notifications/whatever", Json(nullptr)),  // no reply
      makeRequest("2", "tools/list", Json(nullptr)),
  };
  MemTransport t(in);
  const McpResult<void> r = server->run(t);
  ASSERT_TRUE(r.has_value());          // EOF is graceful
  EXPECT_EQ(t.output_.size(), 2u);     // notification produced no output
}

// ---- M4: inputSchema enforcement, completions, CacheableResult --------------------

TEST(ServerM4, ToolInputSchemaValidatesArguments) {
  Builder b;
  Json schema = Json(
      R"({"type":"object","required":["query"],
          "properties":{"query":{"type":"string"},"limit":{"type":"integer","minimum":1}}})");
  Json def;
  def.add("name", Json::str("search"));
  def.add("description", Json::str("searches"));
  def.add("inputSchema", std::move(schema));
  b.tool(std::move(def), [](Ctx&) -> McpResult<Json> { return Ok(Json(R"({"found":true})")); });
  auto server = b.build();

  Out good = serveOnce(server, makeRequest("s1", "tools/call",
                                           Json(R"({"name":"search","arguments":{"query":"q","limit":5}})")));
  ASSERT_NE(good.result, nullptr);

  Out missing = serveOnce(server, makeRequest("s2", "tools/call",
                                              Json(R"({"name":"search","arguments":{}})")));
  ASSERT_NE(missing.error, nullptr);
  EXPECT_EQ(missing.error->error.code, -32602);

  Out negative = serveOnce(server, makeRequest("s3", "tools/call",
                                               Json(R"({"name":"search","arguments":{"query":"q","limit":0}})")));
  ASSERT_NE(negative.error, nullptr);
  EXPECT_EQ(negative.error->error.code, -32602);
}

TEST(ServerM4, CompletionRoute) {
  Builder b;
  b.completions([](Ctx&) -> McpResult<Json> {
    Json result;
    Json completion;
    Json values = Json("[]");
    values.push_back(Json::str("option-1"));
    completion.add("values", std::move(values));
    completion.add("hasMore", Json(false));
    result.add("completion", std::move(completion));
    return Ok(std::move(result));
  });
  auto server = b.build();

  Out o = serveOnce(server, makeRequest("cm1", "completion/complete",
                                        Json(R"({"ref":{"type":"ref/prompt","name":"greet"},
                                              "argument":{"name":"lang","value":"zh"}})")));
  ASSERT_NE(o.result, nullptr);
  EXPECT_EQ(o.result->result["completion"]["values"].size(), 1);

  // capability advertised on discover
  Out d = serveOnce(server, makeRequest("cm2", "server/discover", Json(nullptr)));
  ASSERT_NE(d.result, nullptr);
  EXPECT_TRUE(capabilitiesInclude(d.result->result["capabilities"], "completions"));
}

TEST(ServerM4, ListCacheHints) {
  Builder b;
  b.listCache(60000, "private");
  b.simpleTool("t", "", [](Ctx&) -> McpResult<Json> { return Ok(Json(R"({})")); });
  auto server = b.build();

  Out o = serveOnce(server, makeRequest("h1", "tools/list", Json(nullptr)));
  ASSERT_NE(o.result, nullptr);
  EXPECT_EQ(o.result->result["ttlMs"].toInt64(), 60000);
  EXPECT_EQ(o.result->result["cacheScope"].toString(), "private");

  // default: no cache hints (still needs the tools capability)
  Builder plain;
  plain.simpleTool("t2", "", [](Ctx&) -> McpResult<Json> { return Ok(Json(R"({})")); });
  auto pserver = plain.build();
  Out d = serveOnce(pserver, makeRequest("h2", "tools/list", Json(nullptr)));
  ASSERT_NE(d.result, nullptr);
  EXPECT_TRUE(d.result->result["ttlMs"].isError());
}

// ---- legacy dual-era mode (acceptLegacyClients) ------------------------------------

TEST(ServerLegacy, InitializeHandshakeWhenEnabled) {
  Builder b;
  b.acceptLegacyClients(true)
      .supports(ProtocolVersion::V2025_06_18)
      .supports(ProtocolVersion::Latest);
  auto server = b.build();

  const std::string initLine = encodeRequest(
      RequestId(int64_t(1)), "initialize",
      Json(R"({"protocolVersion":"2025-06-18","capabilities":{},"clientInfo":{"name":"cc","version":"1"}})"));
  Out o = serveOnce(server, initLine);
  ASSERT_NE(o.result, nullptr);
  EXPECT_EQ(o.result->result["protocolVersion"].toString(), "2025-06-18");
  EXPECT_EQ(o.result->result["serverInfo"]["name"].toString(), "mcp-cpp-server");

  // Unsupported legacy version -> -32022 with the supported list.
  Out bad = serveOnce(server, encodeRequest(
      RequestId(int64_t(2)), "initialize",
      Json(R"({"protocolVersion":"1999-01-01","capabilities":{}})")));
  ASSERT_NE(bad.error, nullptr);
  EXPECT_EQ(bad.error->error.code, -32022);
}

TEST(ServerLegacy, RequestsWithoutMetaServedWhenEnabled) {
  Builder b;
  b.acceptLegacyClients(true);
  b.simpleTool("t", "", [](Ctx&) -> McpResult<Json> { return Ok(Json(R"({"ok":true})")); });
  auto server = b.build();

  // No _meta at all: served with default metadata.
  Out o = serveOnce(server, encodeRequest(RequestId(int64_t(1)), "tools/call",
                                          Json(R"({"name":"t"})")));
  ASSERT_NE(o.result, nullptr);
  EXPECT_EQ(o.result->result["ok"].isTrue(), true);

  // Empty _meta object (some AI clients send this): also served.
  Out o2 = serveOnce(server, encodeRequest(RequestId(int64_t(2)), "tools/call",
                                           Json(R"({"name":"t","_meta":{}})")));
  ASSERT_NE(o2.result, nullptr);
  EXPECT_EQ(o2.result->result["ok"].isTrue(), true);

  // Partial _meta (missing required fields): served with defaults in legacy.
  Out o3 = serveOnce(server, encodeRequest(RequestId(int64_t(3)), "tools/call",
                                           Json(R"({"name":"t","_meta":{"foo":1}})")));
  ASSERT_NE(o3.result, nullptr);
  EXPECT_EQ(o3.result->result["ok"].isTrue(), true);

  // _meta present but not an object: still an error.
  Out bad = serveOnce(server, encodeRequest(RequestId(int64_t(4)), "tools/call",
                                            Json(R"({"name":"t","_meta":5})")));
  ASSERT_NE(bad.error, nullptr);
  EXPECT_EQ(bad.error->error.code, -32602);

  // Parameterless request (params:null) - legacy clients call tools/list
  // without params; it must be served with defaults.
  Out o4 = serveOnce(server, encodeRequest(RequestId(int64_t(5)), "tools/list",
                                           Json(nullptr)));
  ASSERT_NE(o4.result, nullptr);
  EXPECT_TRUE(o4.result->result["tools"].isArray());

  // notifications/initialized is a notification: silently ignored.
  MemTransport t({});
  server->processLine(encodeNotification("notifications/initialized", Json(nullptr)), t);
  EXPECT_TRUE(t.output_.empty());
}

TEST(ServerLegacy, PartialMetaStillStrictWhenDisabled) {
  Builder b;  // acceptLegacyClients NOT enabled
  b.simpleTool("t", "", [](Ctx&) -> McpResult<Json> { return Ok(Json(R"({"ok":true})")); });
  auto server = b.build();

  // Empty _meta object fails in strict mode.
  Out o = serveOnce(server, encodeRequest(RequestId(int64_t(1)), "tools/call",
                                          Json(R"({"name":"t","_meta":{}})")));
  ASSERT_NE(o.error, nullptr);
  EXPECT_EQ(o.error->error.code, -32602);

  // No _meta at all fails in strict mode.
  Out o2 = serveOnce(server, encodeRequest(RequestId(int64_t(2)), "tools/call",
                                           Json(R"({"name":"t"})")));
  ASSERT_NE(o2.error, nullptr);
  EXPECT_EQ(o2.error->error.code, -32602);
}

TEST(ServerLegacy, StrictByDefault) {
  Builder b;  // acceptLegacyClients NOT enabled
  auto server = b.build();
  Out o = serveOnce(server, encodeRequest(
      RequestId(int64_t(1)), "initialize",
      Json(R"({"protocolVersion":"2025-06-18","capabilities":{},"clientInfo":{"name":"cc","version":"1"}})")));
  ASSERT_NE(o.error, nullptr);
  EXPECT_EQ(o.error->error.code, -32601);
}
