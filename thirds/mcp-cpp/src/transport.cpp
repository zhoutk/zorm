#include <mcp/transport.hpp>

#include <iostream>
#include <istream>
#include <ostream>
#include <string>

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#include <cstdio>
#endif

namespace mcp {

StreamTransport::StreamTransport(std::istream& in, std::ostream& out) : in_(in), out_(out) {}

McpResult<std::optional<std::string>> StreamTransport::read() {
  std::string line;
  while (true) {
    if (!std::getline(in_, line)) {
      if (in_.eof()) return Ok(std::optional<std::string>(std::nullopt));
      return Err(McpErrorCode::InternalError, "stdio read failed");
    }
    if (!line.empty() && line.back() == '\r') line.pop_back();  // CRLF tolerance
    const bool blank = line.find_first_not_of(" 	") == std::string::npos;
    if (blank) continue;  // skip blank lines (sender must not emit them)
    return Ok(std::optional<std::string>(std::move(line)));
  }
}

McpResult<void> StreamTransport::write(std::string_view message) {
  if (message.find('\n') != std::string_view::npos) {
    return Err(McpErrorCode::InternalError, "transport framing: message contains a newline");
  }
  out_.write(message.data(), static_cast<std::streamsize>(message.size()));
  out_.put('\n');
  if (!out_) return Err(McpErrorCode::InternalError, "stdio write failed");
  return Ok();
}

McpResult<void> StreamTransport::flush() {
  out_.flush();
  if (!out_) return Err(McpErrorCode::InternalError, "stdio flush failed");
  return Ok();
}

// ---- stdio -----------------------------------------------------------------

StdioTransport::StdioTransport() : stream_(std::cin, std::cout) {
#if defined(_WIN32)
  // Keep the message channel binary: no CRLF translation, no UTF-8 mangling
  // through the ANSI code page.
  _setmode(_fileno(stdin), _O_BINARY);
  _setmode(_fileno(stdout), _O_BINARY);
#endif
}

StdioTransport::~StdioTransport() {
  // Make sure a buffered final message reaches the peer before teardown.
  // (Best effort: nothing can be done about a flush failure here.)
  (void)stream_.flush();
}

McpResult<std::optional<std::string>> StdioTransport::read() { return stream_.read(); }

McpResult<void> StdioTransport::write(std::string_view message) { return stream_.write(message); }

McpResult<void> StdioTransport::flush() { return stream_.flush(); }

}  // namespace mcp
