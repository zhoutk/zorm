// Transport framing: newline-delimited messages over injected streams.

#include <mcp/mcp.hpp>

#include <gtest/gtest.h>

#include <iostream>
#include <sstream>
#include <string>

using namespace mcp;

TEST(StreamTransport, ReadSingleLine) {
  std::istringstream in(R"({"jsonrpc":"2.0","id":1,"method":"m"})" "\n");
  std::ostringstream out;
  StreamTransport t(in, out);

  auto msg = t.read();
  ASSERT_TRUE(msg.has_value());
  ASSERT_TRUE(msg->has_value());
  EXPECT_EQ(**msg, R"({"jsonrpc":"2.0","id":1,"method":"m"})");
}

TEST(StreamTransport, EofReturnsNullopt) {
  std::istringstream in("");
  std::ostringstream out;
  StreamTransport t(in, out);

  auto msg = t.read();
  ASSERT_TRUE(msg.has_value());
  EXPECT_FALSE(msg->has_value());  // orderly EOF, not an error
}

TEST(StreamTransport, SkipsBlankLinesAndToleratesCRLF) {
  std::istringstream in("\r\n" R"({"jsonrpc":"2.0","method":"a"})" "\r\n"
                        R"({"jsonrpc":"2.0","method":"b"})" "\n");
  std::ostringstream out;
  StreamTransport t(in, out);

  auto first = t.read();
  ASSERT_TRUE(first.has_value() && first->has_value());
  EXPECT_EQ(**first, R"({"jsonrpc":"2.0","method":"a"})");

  auto second = t.read();
  ASSERT_TRUE(second.has_value() && second->has_value());
  EXPECT_EQ(**second, R"({"jsonrpc":"2.0","method":"b"})");
}

TEST(StreamTransport, EofAfterBlankLines) {
  std::istringstream in("\n\n   \n");
  std::ostringstream out;
  StreamTransport t(in, out);

  auto msg = t.read();
  ASSERT_TRUE(msg.has_value());
  EXPECT_FALSE(msg->has_value());
}

TEST(StreamTransport, WriteAppendsNewlineAndFlushes) {
  std::istringstream in("");
  std::ostringstream out;
  StreamTransport t(in, out);

  const std::string message = encodeNotification("notifications/message", Json(nullptr));
  ASSERT_TRUE(t.write(message).has_value());
  ASSERT_TRUE(t.flush().has_value());
  EXPECT_EQ(out.str(), message + "\n");
}

TEST(StreamTransport, RejectsEmbeddedNewline) {
  std::istringstream in("");
  std::ostringstream out;
  StreamTransport t(in, out);

  auto r = t.write("line1\nline2");
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error().code, McpErrorCode::InternalError);
  EXPECT_TRUE(out.str().empty());
}

TEST(StreamTransport, MessageRoundtrip) {
  // Encode -> stream -> read -> parse must produce the original request.
  std::istringstream in;
  std::ostringstream out;
  StreamTransport t(in, out);

  const std::string text =
      encodeRequest(RequestId(std::string("req-7")), "tools/call", Json(R"({"name":"n"})"));
  ASSERT_TRUE(t.write(text).has_value());

  std::istringstream in2(out.str());
  StreamTransport reader(in2, out);
  auto line = reader.read();
  ASSERT_TRUE(line.has_value() && line->has_value());

  ParsedMessage parsed = parseMessage(**line);
  const auto* req = std::get_if<JsonRpcRequest>(&parsed);
  ASSERT_NE(req, nullptr);
  EXPECT_EQ(std::get<std::string>(req->id), "req-7");
  EXPECT_EQ(req->method, "tools/call");
}
