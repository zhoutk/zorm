// RFC 4648 base64 (standard alphabet, padding). Private to the library;
// used by the Streamable HTTP value-encoding rules ("=?base64?...?=").
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace mcp::detail {

std::string base64Encode(std::string_view raw);
// Returns false on any malformed input; output untouched in that case.
bool base64Decode(std::string_view text, std::string& out);

// True when the value can be carried as a plain HTTP header value:
// visible ASCII (0x21-0x7E), space, horizontal tab; no leading/trailing space.
bool isPlainHeaderValue(std::string_view value);

// Wraps `raw` into the spec's sentinel form "=?base64?{encoded}?=" when it
// cannot be sent plain; returns it unchanged otherwise.
std::string encodeHeaderValue(std::string_view raw);
// Inverse of encodeHeaderValue; plain values pass through unchanged.
bool decodeHeaderValue(std::string_view value, std::string& out);

}  // namespace mcp::detail
