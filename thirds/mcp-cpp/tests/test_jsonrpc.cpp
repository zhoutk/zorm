// JSON-RPC message layer: strict decode per MCP 2026-07-28 deltas over
// JSON-RPC 2.0, plus encode/decode roundtrips.

#include <mcp/mcp.hpp>

#include <gtest/gtest.h>

#include <string>
#include <variant>

using namespace mcp;

namespace {

const JsonRpcRequest* asRequest(const ParsedMessage& m) {
  return std::get_if<JsonRpcRequest>(&m);
}
const JsonRpcNotification* asNotification(const ParsedMessage& m) {
  return std::get_if<JsonRpcNotification>(&m);
}
const JsonRpcResult* asResult(const ParsedMessage& m) {
  return std::get_if<JsonRpcResult>(&m);
}
const JsonRpcErrorResponse* asErrorResponse(const ParsedMessage& m) {
  return std::get_if<JsonRpcErrorResponse>(&m);
}
const InvalidMessage* asInvalid(const ParsedMessage& m) {
  return std::get_if<InvalidMessage>(&m);
}

}  // namespace

// ---- valid incoming messages -------------------------------------------------

TEST(ParseMessage, RequestWithIntegerId) {
  auto m = parseMessage(R"({"jsonrpc":"2.0","id":1,"method":"tools/list"})");
  const auto* r = asRequest(m);
  ASSERT_NE(r, nullptr);
  EXPECT_EQ(std::get<int64_t>(r->id), 1);
  EXPECT_EQ(r->method, "tools/list");
  EXPECT_TRUE(r->params.isNull());
}

TEST(ParseMessage, RequestWithStringIdAndParams) {
  auto m = parseMessage(
      R"({"jsonrpc":"2.0","id":"abc","method":"tools/call","params":{"name":"t"}})");
  const auto* r = asRequest(m);
  ASSERT_NE(r, nullptr);
  EXPECT_EQ(std::get<std::string>(r->id), "abc");
  ASSERT_TRUE(r->params.isObject());
  EXPECT_EQ(r->params["name"].toString(), "t");
}

TEST(ParseMessage, RequestParamsAbsentBecomesNull) {
  auto m = parseMessage(R"({"jsonrpc":"2.0","id":7,"method":"x"})");
  const auto* r = asRequest(m);
  ASSERT_NE(r, nullptr);
  EXPECT_TRUE(r->params.isNull());
}

TEST(ParseMessage, RequestExplicitNullParamsBecomesNull) {
  auto m = parseMessage(R"({"jsonrpc":"2.0","id":7,"method":"x","params":null})");
  const auto* r = asRequest(m);
  ASSERT_NE(r, nullptr);
  EXPECT_TRUE(r->params.isNull());
}

TEST(ParseMessage, ExtremeInt64Ids) {
  for (const int64_t extreme : {int64_t(INT64_MAX), int64_t(INT64_MIN), int64_t(0)}) {
    auto m = parseMessage(std::string(R"({"jsonrpc":"2.0","id":)") + std::to_string(extreme) +
                          R"(,"method":"m"})");
    const auto* r = asRequest(m);
    ASSERT_NE(r, nullptr) << extreme;
    EXPECT_EQ(std::get<int64_t>(r->id), extreme);
  }
}

TEST(ParseMessage, Notification) {
  auto m = parseMessage(R"({"jsonrpc":"2.0","method":"notifications/cancelled"})");
  const auto* n = asNotification(m);
  ASSERT_NE(n, nullptr);
  EXPECT_EQ(n->method, "notifications/cancelled");
}

TEST(ParseMessage, ResultResponse) {
  auto m = parseMessage(
      R"({"jsonrpc":"2.0","id":5,"result":{"resultType":"complete","tools":[]}})");
  const auto* r = asResult(m);
  ASSERT_NE(r, nullptr);
  EXPECT_EQ(std::get<int64_t>(r->id), 5);
  EXPECT_EQ(r->result["resultType"].toString(), "complete");
}

TEST(ParseMessage, ErrorResponse) {
  auto m = parseMessage(
      R"({"jsonrpc":"2.0","id":"x","error":{"code":-32602,"message":"bad","data":{"k":1}}})");
  const auto* r = asErrorResponse(m);
  ASSERT_NE(r, nullptr);
  ASSERT_TRUE(r->id.has_value());
  EXPECT_EQ(std::get<std::string>(*r->id), "x");
  EXPECT_EQ(r->error.code, -32602);
  EXPECT_EQ(r->error.message, "bad");
  EXPECT_EQ(r->error.data["k"].toInt64(), 1);
}

TEST(ParseMessage, ErrorResponseWithoutId) {
  auto m = parseMessage(R"({"jsonrpc":"2.0","error":{"code":-32700,"message":"parse"}})");
  const auto* r = asErrorResponse(m);
  ASSERT_NE(r, nullptr);
  EXPECT_FALSE(r->id.has_value());
  EXPECT_EQ(r->error.code, -32700);
}

// ---- invalid incoming messages ------------------------------------------------

TEST(ParseMessage, UnparseableJsonIsParseError) {
  for (const char* text : {"", "{", "not json", R"({"jsonrpc":})", "{\"a\":"}) {
    ParsedMessage inv_msg = parseMessage(text);
  const auto* inv = asInvalid(inv_msg);
    ASSERT_NE(inv, nullptr) << text;
    EXPECT_EQ(inv->code, McpErrorCode::ParseError);
    EXPECT_FALSE(inv->id.has_value());
  }
}

TEST(ParseMessage, NonObjectIsInvalidRequest) {
  // Batches (arrays) are not part of MCP; scalars are never valid.
  for (const char* text : {"[]", R"([{"jsonrpc":"2.0","id":1,"method":"m"}])", "42", "\"x\"",
                           "true", "null"}) {
    ParsedMessage inv_msg = parseMessage(text);
  const auto* inv = asInvalid(inv_msg);
    ASSERT_NE(inv, nullptr) << text;
    EXPECT_EQ(inv->code, McpErrorCode::InvalidRequest) << text;
  }
}

TEST(ParseMessage, MissingOrWrongJsonrpcMember) {
  for (const char* text : {R"({"id":1,"method":"m"})", R"({"jsonrpc":"1.0","id":1,"method":"m"})",
                           R"({"jsonrpc":2,"id":1,"method":"m"})"}) {
    ParsedMessage inv_msg = parseMessage(text);
  const auto* inv = asInvalid(inv_msg);
    ASSERT_NE(inv, nullptr) << text;
    EXPECT_EQ(inv->code, McpErrorCode::InvalidRequest) << text;
  }
}

TEST(ParseMessage, MethodMustBeNonEmptyString) {
  for (const char* text : {R"({"jsonrpc":"2.0","id":1,"method":42})",
                           R"({"jsonrpc":"2.0","id":1,"method":""})"}) {
    ParsedMessage inv_msg = parseMessage(text);
  const auto* inv = asInvalid(inv_msg);
    ASSERT_NE(inv, nullptr) << text;
    EXPECT_EQ(inv->code, McpErrorCode::InvalidRequest) << text;
  }
}

TEST(ParseMessage, NullIdIsRejected) {
  // id member present but null: MCP forbids null ids; JSON-RPC 2.0 would
  // read this as a notification. We reject so the mismatch is loud.
  ParsedMessage inv_msg = parseMessage(R"({"jsonrpc":"2.0","id":null,"method":"m"})");
  const auto* inv = asInvalid(inv_msg);
  ASSERT_NE(inv, nullptr);
  EXPECT_EQ(inv->code, McpErrorCode::InvalidRequest);
}

TEST(ParseMessage, BoolObjectArrayIdsRejected) {
  for (const char* text :
       {R"({"jsonrpc":"2.0","id":true,"method":"m"})",
        R"({"jsonrpc":"2.0","id":{"a":1},"method":"m"})",
        R"({"jsonrpc":"2.0","id":[1],"method":"m"})"}) {
    ParsedMessage inv_msg = parseMessage(text);
  const auto* inv = asInvalid(inv_msg);
    ASSERT_NE(inv, nullptr) << text;
    EXPECT_EQ(inv->code, McpErrorCode::InvalidRequest) << text;
  }
}

TEST(ParseMessage, FractionalIdRejected) {
  ParsedMessage inv_msg = parseMessage(R"({"jsonrpc":"2.0","id":1.5,"method":"m"})");
  const auto* inv = asInvalid(inv_msg);
  ASSERT_NE(inv, nullptr);
  EXPECT_EQ(inv->code, McpErrorCode::InvalidRequest);
}

TEST(ParseMessage, IntegralDoubleIdAccepted) {
  auto m = parseMessage(R"({"jsonrpc":"2.0","id":2.0,"method":"m"})");
  const auto* r = asRequest(m);
  ASSERT_NE(r, nullptr);
  EXPECT_EQ(std::get<int64_t>(r->id), 2);
}

TEST(ParseMessage, ArrayParamsRejected) {
  ParsedMessage inv_msg = parseMessage(R"({"jsonrpc":"2.0","id":1,"method":"m","params":[1]})");
  const auto* inv = asInvalid(inv_msg);
  ASSERT_NE(inv, nullptr);
  EXPECT_EQ(inv->code, McpErrorCode::InvalidRequest);
}

TEST(ParseMessage, MessageWithoutMethodOrId) {
  ParsedMessage inv_msg = parseMessage(R"({"jsonrpc":"2.0","params":{}})");
  const auto* inv = asInvalid(inv_msg);
  ASSERT_NE(inv, nullptr);
  EXPECT_EQ(inv->code, McpErrorCode::InvalidRequest);
}

TEST(ParseMessage, ResponseMustHaveExactlyOneOfResultError) {
  for (const char* text : {R"({"jsonrpc":"2.0","id":1})",
                           R"({"jsonrpc":"2.0","id":1,"result":{},"error":{"code":1,"message":"m"}})"}) {
    ParsedMessage inv_msg = parseMessage(text);
  const auto* inv = asInvalid(inv_msg);
    ASSERT_NE(inv, nullptr) << text;
    EXPECT_EQ(inv->code, McpErrorCode::InvalidRequest) << text;
  }
}

TEST(ParseMessage, MalformedErrorObject) {
  for (const char* text :
       {R"({"jsonrpc":"2.0","id":1,"error":{"code":"x","message":"m"}})",
        R"({"jsonrpc":"2.0","id":1,"error":{"code":-1}})",
        R"({"jsonrpc":"2.0","id":1,"error":{"message":"m"}})",
        R"({"jsonrpc":"2.0","id":1,"error":{"code":-1,"message":5}})",
        R"({"jsonrpc":"2.0","id":1,"error":5})"}) {
    ParsedMessage inv_msg = parseMessage(text);
  const auto* inv = asInvalid(inv_msg);
    ASSERT_NE(inv, nullptr) << text;
    EXPECT_EQ(inv->code, McpErrorCode::InvalidRequest) << text;
  }
}

TEST(ParseMessage, FractionalErrorCodeRejected) {
  ParsedMessage inv_msg = parseMessage(R"({"jsonrpc":"2.0","id":1,"error":{"code":-1.5,"message":"m"}})");
  const auto* inv = asInvalid(inv_msg);
  ASSERT_NE(inv, nullptr);
}

TEST(ParseMessage, InvalidRequestWithRecoverableIdKeepsIt) {
  // params array with a valid id: the responder can echo the id back.
  ParsedMessage inv_msg = parseMessage(R"({"jsonrpc":"2.0","id":"keep","method":"m","params":[]})");
  const auto* inv = asInvalid(inv_msg);
  ASSERT_NE(inv, nullptr);
  ASSERT_TRUE(inv->id.has_value());
  EXPECT_EQ(std::get<std::string>(*inv->id), "keep");
}

// ---- encoding ------------------------------------------------------------------

TEST(Encode, RequestRoundtrip) {
  const std::string text = encodeRequest(RequestId(int64_t(42)), "tools/call",
                                         Json(R"({"name":"t"})"));
  ParsedMessage r_msg = parseMessage(text);
  const auto* r = asRequest(r_msg);
  ASSERT_NE(r, nullptr);
  EXPECT_EQ(std::get<int64_t>(r->id), 42);
  EXPECT_EQ(r->method, "tools/call");
  EXPECT_EQ(r->params["name"].toString(), "t");
}

TEST(Encode, RequestOmitsNullParams) {
  const std::string text = encodeRequest(RequestId(int64_t(1)), "m", Json(nullptr));
  EXPECT_EQ(text.find("params"), std::string::npos);
}

TEST(Encode, NotificationNeverCarriesId) {
  const std::string text = encodeNotification("notifications/cancelled", Json(nullptr));
  EXPECT_EQ(text.find("\"id\""), std::string::npos);
  ParsedMessage n_msg = parseMessage(text);
  EXPECT_NE(asNotification(n_msg), nullptr);
}

TEST(Encode, StringValuesAreNeverSniffedAsJson) {
  // A string value that looks like JSON must stay a string (zjson sniffing).
  const std::string text = encodeRequest(RequestId(std::string("id")), "m",
                                         Json(R"({"v":"[1,2]"})"));
  ParsedMessage r_msg = parseMessage(text);
  const auto* r = asRequest(r_msg);
  ASSERT_NE(r, nullptr);
  EXPECT_TRUE(r->params["v"].isString());
  EXPECT_EQ(r->params["v"].toString(), "[1,2]");
}

TEST(Encode, ResultRoundtrip) {
  Json result;
  result.add("resultType", Json::str("complete"));
  result.add("tools", Json("[]"));
  const std::string text = encodeResultResponse(RequestId(int64_t(9)), result);
  ParsedMessage r_msg = parseMessage(text);
  const auto* r = asResult(r_msg);
  ASSERT_NE(r, nullptr);
  EXPECT_EQ(std::get<int64_t>(r->id), 9);
  EXPECT_EQ(r->result["resultType"].toString(), "complete");
}

TEST(Encode, ErrorResponseOmitsUnreadableId) {
  const std::string text =
      encodeErrorResponse(std::nullopt, parseErrorValue("Parse error"));
  EXPECT_EQ(text.find("\"id\""), std::string::npos);
  ParsedMessage r_msg = parseMessage(text);
  const auto* r = asErrorResponse(r_msg);
  ASSERT_NE(r, nullptr);
  EXPECT_FALSE(r->id.has_value());
  EXPECT_EQ(r->error.code, -32700);
}

TEST(Encode, ErrorResponseIncludesData) {
  Json data;
  data.add("supported", Json(R"(["2026-07-28"])"));
  JsonRpcErrorValue v;
  v.code = -32022;
  v.message = "Unsupported protocol version";
  v.data = data;
  const std::string text = encodeErrorResponse(RequestId(int64_t(1)), v);
  ParsedMessage r_msg = parseMessage(text);
  const auto* r = asErrorResponse(r_msg);
  ASSERT_NE(r, nullptr);
  EXPECT_EQ(r->error.data["supported"].size(), 1);
}

TEST(Encode, NoEmbeddedNewlines) {
  // Framing contract: one message per line.
  Json params;
  params.add("a", Json::str("line1\nline2"));
  const std::string text = encodeRequest(RequestId(int64_t(1)), "m", params);
  EXPECT_EQ(text.find('\n'), std::string::npos);  // escaped by the JSON writer
}
