#pragma once
// ---------------------------------------------------------------------------
// mcp/transport.hpp - newline-delimited JSON-RPC framing over byte streams.
//
// Framing follows the stdio binding (basic/transports/stdio.md): one JSON-RPC
// message per line, no embedded newlines, logs never on the message channel.
// The wire format is transport-agnostic - any reliable bidirectional byte
// stream (socket, pipe) can host StreamTransport.
//
// Streams are injected, which keeps the transport fully testable in memory;
// StdioTransport just wires std::cin/std::cout (switched to binary mode on
// Windows).
// ---------------------------------------------------------------------------

#include <mcp/error.hpp>

#include <iosfwd>
#include <optional>
#include <string>
#include <string_view>

namespace mcp {

class ITransport {
 public:
  virtual ~ITransport() = default;

  // Reads one newline-delimited message. nullopt payload = orderly EOF.
  virtual McpResult<std::optional<std::string>> read() = 0;

  // Writes one message (a trailing newline is appended; input must not
  // contain one).
  virtual McpResult<void> write(std::string_view message) = 0;

  // Flushes the output channel.
  virtual McpResult<void> flush() = 0;

  // Optional per-message header injection (Streamable HTTP mirrored headers).
  // No-op on transports without a header layer (stdio).
  virtual void setHeader(std::string /*name*/, std::string /*value*/) {}
};

// Framing over arbitrary C++ streams. Skips empty/blank input lines (the
// sender MUST NOT emit them, but trailing newlines happen in the wild) and
// tolerates CRLF.
class StreamTransport final : public ITransport {
 public:
  StreamTransport(std::istream& in, std::ostream& out);

  McpResult<std::optional<std::string>> read() override;
  McpResult<void> write(std::string_view message) override;
  McpResult<void> flush() override;

 private:
  std::istream& in_;
  std::ostream& out_;
};

// The canonical stdio binding. On Windows both standard streams are switched
// to binary mode so "\n" stays "\n" and UTF-8 passes through unmangled.
class StdioTransport final : public ITransport {
 public:
  StdioTransport();  // configures stdin/stdout modes
  ~StdioTransport() override;

  McpResult<std::optional<std::string>> read() override;
  McpResult<void> write(std::string_view message) override;
  McpResult<void> flush() override;

 private:
  StreamTransport stream_;
};

}  // namespace mcp
