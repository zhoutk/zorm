// Core value types: RequestId, Implementation, per-request _meta, capability
// probes, resultType defaults.

#include <mcp/mcp.hpp>

#include <gtest/gtest.h>

#include <string>
#include <variant>

using namespace mcp;

namespace {

const JsonRpcRequest* asRequest(const ParsedMessage& m) {
  return std::get_if<JsonRpcRequest>(&m);
}

}  // namespace

// ---- RequestId -----------------------------------------------------------------

TEST(RequestId, RoundtripThroughWire) {
  for (const RequestId& id : {RequestId(int64_t(0)), RequestId(int64_t(INT64_MAX)),
                              RequestId(int64_t(INT64_MIN)), RequestId(std::string("")),
                              RequestId(std::string("req-1"))}) {
        const Json j = encodeRequestId(id);
        auto back = decodeRequestId(j);
        ASSERT_TRUE(back.has_value());
        EXPECT_EQ(back->index(), id.index());
        if (id.index() == 0) {
          EXPECT_EQ(std::get<int64_t>(*back), std::get<int64_t>(id));
        } else {
          EXPECT_EQ(std::get<std::string>(*back), std::get<std::string>(id));
        }
  }
}

TEST(RequestId, RejectsNonIdValues) {
  EXPECT_FALSE(decodeRequestId(Json(nullptr)).has_value());
  EXPECT_FALSE(decodeRequestId(Json(true)).has_value());
  EXPECT_FALSE(decodeRequestId(Json(1.5)).has_value());
  EXPECT_TRUE(decodeRequestId(Json(1)).has_value());
}

TEST(RequestId, NumericStringStaysString) {
  // "1" as a wire value is a string id - never promoted to an integer.
  auto id = decodeRequestId(Json::str("1"));
  ASSERT_TRUE(id.has_value());
  EXPECT_EQ(id->index(), 1);  // std::string alternative
}

// ---- Implementation --------------------------------------------------------------

TEST(Implementation, RoundtripMinimal) {
  Implementation impl;
  impl.name = "zorm";
  impl.version = "0.1.0";
  const Json j = encodeImplementation(impl);
  auto back = decodeImplementation(j);
  ASSERT_TRUE(back.has_value());
  EXPECT_EQ(back->name, "zorm");
  EXPECT_EQ(back->version, "0.1.0");
  EXPECT_TRUE(back->title.empty());
  EXPECT_TRUE(back->icons.isNull());
}

TEST(Implementation, RoundtripFull) {
  Implementation impl;
  impl.name = "mcp-cpp";
  impl.title = "MCP C++ SDK";
  impl.version = "1.0.0";
  impl.description = "desc";
  impl.websiteUrl = "https://example.com";
  Json icons = Json("[]");
  icons.push_back(Json::str("https://example.com/icon.png"));
  impl.icons = icons;

  auto back = decodeImplementation(encodeImplementation(impl));
  ASSERT_TRUE(back.has_value());
  EXPECT_EQ(back->title, "MCP C++ SDK");
  EXPECT_EQ(back->websiteUrl, "https://example.com");
  ASSERT_TRUE(back->icons.isArray());
  EXPECT_EQ(back->icons.size(), 1);
}

TEST(Implementation, DecodeRequiresNameAndVersion) {
  EXPECT_FALSE(decodeImplementation(Json(R"({"version":"1"})")).has_value());
  EXPECT_FALSE(decodeImplementation(Json(R"({"name":"x"})")).has_value());
  EXPECT_FALSE(decodeImplementation(Json(R"({"name":1,"version":"1"})")).has_value());
  EXPECT_FALSE(decodeImplementation(Json(R"("string")")).has_value());
}

// ---- per-request _meta -------------------------------------------------------------

TEST(RequestMeta, DecodeValid) {
  Json caps = Json(R"({"tools":{}})");
  Json params = encodeRequestMeta("2026-07-28", caps, std::nullopt);
  auto meta = decodeRequestMeta(params);
  ASSERT_TRUE(meta.has_value());
  EXPECT_EQ(meta->protocolVersion, "2026-07-28");
  EXPECT_TRUE(capabilitiesInclude(meta->clientCapabilities, "tools"));
  EXPECT_FALSE(meta->clientInfo.has_value());
}

TEST(RequestMeta, DecodeWithClientInfo) {
  Implementation impl;
  impl.name = "client";
  impl.version = "2.0";
  Json params = encodeRequestMeta("2025-11-25", Json(R"({})"), impl);
  auto meta = decodeRequestMeta(params);
  ASSERT_TRUE(meta.has_value());
  ASSERT_TRUE(meta->clientInfo.has_value());
  EXPECT_EQ(meta->clientInfo->name, "client");
}

TEST(RequestMeta, MissingRequiredFieldsAreInvalidParams) {
  // No _meta at all.
  auto r1 = decodeRequestMeta(Json(R"({"name":"t"})"));
  ASSERT_FALSE(r1.has_value());
  EXPECT_EQ(r1.error().code, McpErrorCode::InvalidParams);

  // Missing protocolVersion.
  auto r2 = decodeRequestMeta(
      Json(R"({"_meta":{"io.modelcontextprotocol\/clientCapabilities":{}}})"));
  ASSERT_FALSE(r2.has_value());

  // Missing clientCapabilities.
  auto r3 = decodeRequestMeta(
      Json(R"({"_meta":{"io.modelcontextprotocol\/protocolVersion":"2026-07-28"}})"));
  ASSERT_FALSE(r3.has_value());

  // _meta not an object.
  auto r4 = decodeRequestMeta(Json(R"({"_meta":5})"));
  ASSERT_FALSE(r4.has_value());
}
TEST(RequestMeta, RelaxedDecodeServesPartialMeta) {
  // No _meta at all: defaults + complete=false.
  auto r1 = decodeRequestMetaRelaxed(Json(R"({"name":"t"})"));
  ASSERT_TRUE(r1.has_value());
  EXPECT_FALSE(r1->complete);
  EXPECT_EQ(r1->protocolVersion, "2026-07-28");
  EXPECT_TRUE(r1->clientCapabilities.isObject());

  // Empty _meta object (what some AI clients send): served with defaults.
  auto r2 = decodeRequestMetaRelaxed(Json(R"({"_meta":{}})"));
  ASSERT_TRUE(r2.has_value());
  EXPECT_FALSE(r2->complete);
  EXPECT_EQ(r2->protocolVersion, "2026-07-28");

  // Partial: only clientCapabilities present.
  auto r3 = decodeRequestMetaRelaxed(
      Json(R"({"_meta":{"io.modelcontextprotocol\/clientCapabilities":{"tools":{}}}})"));
  ASSERT_TRUE(r3.has_value());
  EXPECT_FALSE(r3->complete);
  EXPECT_TRUE(capabilitiesInclude(r3->clientCapabilities, "tools"));

  // Partial: only protocolVersion present.
  auto r4 = decodeRequestMetaRelaxed(
      Json(R"({"_meta":{"io.modelcontextprotocol\/protocolVersion":"2025-11-25"}})"));
  ASSERT_TRUE(r4.has_value());
  EXPECT_FALSE(r4->complete);
  EXPECT_EQ(r4->protocolVersion, "2025-11-25");

  // Complete meta: complete=true, fields preserved.
  auto r5 = decodeRequestMetaRelaxed(
      Json(R"({"_meta":{"io.modelcontextprotocol\/protocolVersion":"2025-06-18","io.modelcontextprotocol\/clientCapabilities":{"tools":{}}}})"));
  ASSERT_TRUE(r5.has_value());
  EXPECT_TRUE(r5->complete);
  EXPECT_EQ(r5->protocolVersion, "2025-06-18");

  // _meta present but not an object: still an error (nothing to fall back on).
  auto r6 = decodeRequestMetaRelaxed(Json(R"({"_meta":5})"));
  ASSERT_FALSE(r6.has_value());
  EXPECT_EQ(r6.error().code, McpErrorCode::InvalidParams);
}

TEST(RequestMeta, RelaxedDecodeHandlesNullParams) {
  // Parameterless methods (tools/list etc.) send params:null; legacy mode
  // must serve them with defaults instead of failing.
  auto r1 = decodeRequestMetaRelaxed(Json(nullptr));
  ASSERT_TRUE(r1.has_value());
  EXPECT_FALSE(r1->complete);
  EXPECT_EQ(r1->protocolVersion, "2026-07-28");
  EXPECT_TRUE(r1->clientCapabilities.isObject());

  // Non-object, non-null params are still rejected.
  auto r2 = decodeRequestMetaRelaxed(Json(R"(5)"));
  ASSERT_FALSE(r2.has_value());
  EXPECT_EQ(r2.error().code, McpErrorCode::InvalidParams);
}
TEST(RequestMeta, WireRoundtripThroughFullRequest) {
  // encode meta -> embed as params -> full request -> decode back.
  Json params = encodeRequestMeta("2026-07-28", Json(R"({"roots":{}})"), std::nullopt);
  const std::string text = encodeRequest(RequestId(int64_t(1)), "tools/list", params);
  ParsedMessage req_msg = parseMessage(text);
  const auto* req = asRequest(req_msg);
  ASSERT_NE(req, nullptr);
  auto meta = decodeRequestMeta(req->params);
  ASSERT_TRUE(meta.has_value());
  EXPECT_EQ(meta->protocolVersion, "2026-07-28");
  EXPECT_TRUE(capabilitiesInclude(meta->clientCapabilities, "roots"));
}

// ---- capability probes ---------------------------------------------------------------

TEST(Capabilities, IncludeProbe) {
  const Json caps = Json(R"({"tools":{"listChanged":true},"prompts":{},"experimental":{"x":{}}})");
  EXPECT_TRUE(capabilitiesInclude(caps, "tools"));
  EXPECT_TRUE(capabilitiesInclude(caps, "prompts"));
  EXPECT_FALSE(capabilitiesInclude(caps, "resources"));
  // A bare true also counts as "advertised" (matches TS SDK truthiness use).
  EXPECT_TRUE(capabilitiesInclude(Json(R"({"logging":true})"), "logging"));
  EXPECT_FALSE(capabilitiesInclude(Json(R"({"logging":false})"), "logging"));
  EXPECT_FALSE(capabilitiesInclude(Json(nullptr), "tools"));
}

TEST(Capabilities, ExtensionProbe) {
  const Json caps =
      Json(R"({"tools":{},"extensions":{"io.modelcontextprotocol\/ui":{"mimeTypes":["text/html"]}}})");
  EXPECT_TRUE(capabilitiesIncludeExtension(caps, "io.modelcontextprotocol/ui"));
  EXPECT_FALSE(capabilitiesIncludeExtension(caps, "io.modelcontextprotocol/tasks"));
  EXPECT_FALSE(capabilitiesIncludeExtension(Json(R"({"extensions":{}})"), "anything"));
}

// ---- resultType ------------------------------------------------------------------------

TEST(ResultType, DefaultsToCompleteWhenAbsent) {
  EXPECT_EQ(resultTypeOf(Json(R"({"tools":[]})")), "complete");
  EXPECT_EQ(resultTypeOf(Json(R"({})")), "complete");
  EXPECT_EQ(resultTypeOf(Json(nullptr)), "complete");  // defensive
}

TEST(ResultType, ReadsPolymorphicValues) {
  EXPECT_EQ(resultTypeOf(Json(R"({"resultType":"complete"})")), "complete");
  EXPECT_EQ(resultTypeOf(Json(R"({"resultType":"input_required"})")), "input_required");
}
