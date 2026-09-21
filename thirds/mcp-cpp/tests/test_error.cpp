// Error model: spec-pinned code values, reserved range, result semantics.

#include <mcp/mcp.hpp>

#include <gtest/gtest.h>

#include <string>
#include <utility>

using namespace mcp;

// Wire values are frozen by the specification - pin them at compile time.
static_assert(static_cast<int32_t>(McpErrorCode::ParseError) == -32700);
static_assert(static_cast<int32_t>(McpErrorCode::InvalidRequest) == -32600);
static_assert(static_cast<int32_t>(McpErrorCode::MethodNotFound) == -32601);
static_assert(static_cast<int32_t>(McpErrorCode::InvalidParams) == -32602);
static_assert(static_cast<int32_t>(McpErrorCode::InternalError) == -32603);
static_assert(static_cast<int32_t>(McpErrorCode::HeaderMismatch) == -32020);
static_assert(static_cast<int32_t>(McpErrorCode::MissingRequiredClientCapability) == -32021);
static_assert(static_cast<int32_t>(McpErrorCode::UnsupportedProtocolVersion) == -32022);

TEST(ErrorCode, ReservedRange) {
  EXPECT_TRUE(isMcpReservedCode(-32020));
  EXPECT_TRUE(isMcpReservedCode(-32022));
  EXPECT_TRUE(isMcpReservedCode(-32099));
  EXPECT_FALSE(isMcpReservedCode(-32019));   // legacy implementation range
  EXPECT_FALSE(isMcpReservedCode(-32100));   // outside on the other side
  EXPECT_FALSE(isMcpReservedCode(-32600));   // JSON-RPC standard range
}

TEST(Result, OkPath) {
  McpResult<int> r = Ok(42);
  ASSERT_TRUE(r.has_value());
  EXPECT_EQ(r.value(), 42);
  EXPECT_TRUE(static_cast<bool>(r));
}

TEST(Result, ErrPath) {
  McpResult<int> r = Err(McpErrorCode::InvalidParams, "bad input");
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error().code, McpErrorCode::InvalidParams);
  EXPECT_EQ(r.error().message, "bad input");
  EXPECT_FALSE(static_cast<bool>(r));
}

TEST(Result, ErrFromMcpError) {
  McpError e{McpErrorCode::HeaderMismatch, "header mismatch"};
  McpResult<int> r = Err(std::move(e));
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error().code, McpErrorCode::HeaderMismatch);
}

TEST(Result, MoveOutValue) {
  McpResult<std::string> r = Ok(std::string("hello"));
  std::string v = std::move(r).value();
  EXPECT_EQ(v, "hello");
}

TEST(Result, VoidOk) {
  McpResult<void> r = Ok();
  EXPECT_TRUE(r.has_value());
  r.value();  // no-op, must compile
}

TEST(Result, VoidErr) {
  McpResult<void> r = Err(McpErrorCode::InternalError, "boom");
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error().code, McpErrorCode::InternalError);
  EXPECT_EQ(r.error().message, "boom");
}

// A function returning via the Err factory in every shape a handler would.
namespace {

McpResult<int> positive(int v) {
  if (v <= 0) return Err(McpErrorCode::InvalidParams, "must be positive");
  return Ok(v);
}

McpResult<void> check(int v) {
  if (v <= 0) return Err(McpErrorCode::InvalidParams, "must be positive");
  return Ok();
}

// Non-default-constructible T must work (Result never default-constructs).
class NoDefault {
 public:
  explicit NoDefault(int v) : v_(v) {}
  int v_;
};

McpResult<NoDefault> makeNoDefault(int v) {
  if (v < 0) return Err(McpErrorCode::InvalidParams, "negative");
  return Ok(NoDefault(v));
}

}  // namespace

TEST(Result, RealisticControlFlow) {
  auto a = positive(3);
  ASSERT_TRUE(a.has_value());
  EXPECT_EQ(a.value(), 3);

  auto b = positive(-1);
  ASSERT_FALSE(b.has_value());
  EXPECT_EQ(b.error().message, "must be positive");

  EXPECT_TRUE(check(1).has_value());
  EXPECT_FALSE(check(0).has_value());

  auto c = makeNoDefault(-5);
  ASSERT_FALSE(c.has_value());
  auto d = makeNoDefault(7);
  ASSERT_TRUE(d.has_value());
  EXPECT_EQ(d.value().v_, 7);
}

#ifdef MCP_HAS_STD_EXPECTED
TEST(Result, StdExpectedMode) {
  // Smoke-check that the alias really is std::expected in this mode.
  McpResult<int> r = Ok(1);
  EXPECT_TRUE(r.has_value());
}
#endif
