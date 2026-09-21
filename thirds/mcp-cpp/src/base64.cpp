#include "base64.h"

#include <cctype>

namespace mcp::detail {
namespace {

constexpr char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

int alphabetIndex(char ch) {
  if (ch >= 'A' && ch <= 'Z') return ch - 'A';
  if (ch >= 'a' && ch <= 'z') return ch - 'a' + 26;
  if (ch >= '0' && ch <= '9') return ch - '0' + 52;
  if (ch == '+') return 62;
  if (ch == '/') return 63;
  return -1;
}

constexpr const char* kSentinelPrefix = "=?base64?";
constexpr const char* kSentinelSuffix = "?=";

}  // namespace

std::string base64Encode(std::string_view raw) {
  std::string out;
  out.reserve(((raw.size() + 2) / 3) * 4);
  size_t i = 0;
  while (i + 2 < raw.size()) {
    const uint32_t n = (static_cast<unsigned char>(raw[i]) << 16) |
                       (static_cast<unsigned char>(raw[i + 1]) << 8) |
                       static_cast<unsigned char>(raw[i + 2]);
    out.push_back(kAlphabet[(n >> 18) & 63]);
    out.push_back(kAlphabet[(n >> 12) & 63]);
    out.push_back(kAlphabet[(n >> 6) & 63]);
    out.push_back(kAlphabet[n & 63]);
    i += 3;
  }
  const size_t rest = raw.size() - i;
  if (rest == 1) {
    const uint32_t n = static_cast<unsigned char>(raw[i]) << 16;
    out.push_back(kAlphabet[(n >> 18) & 63]);
    out.push_back(kAlphabet[(n >> 12) & 63]);
    out.push_back('=');
    out.push_back('=');
  } else if (rest == 2) {
    const uint32_t n = (static_cast<unsigned char>(raw[i]) << 16) |
                       (static_cast<unsigned char>(raw[i + 1]) << 8);
    out.push_back(kAlphabet[(n >> 18) & 63]);
    out.push_back(kAlphabet[(n >> 12) & 63]);
    out.push_back(kAlphabet[(n >> 6) & 63]);
    out.push_back('=');
  }
  return out;
}

bool base64Decode(std::string_view text, std::string& out) {
  out.clear();
  // Strip padding; reject stray characters and impossible lengths.
  while (!text.empty() && text.back() == '=') text.remove_suffix(1);
  out.reserve((text.size() * 3) / 4 + 3);
  uint32_t acc = 0;
  int bits = 0;
  for (const char ch : text) {
    const int v = alphabetIndex(ch);
    if (v < 0) return false;
    acc = (acc << 6) | static_cast<uint32_t>(v);
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      out.push_back(static_cast<char>((acc >> bits) & 0xFF));
    }
  }
  return bits == 0 || bits == 2 || bits == 4;  // only full 6-bit groups remain
}

bool isPlainHeaderValue(std::string_view value) {
  if (value.empty()) return false;
  if (value.front() == ' ' || value.front() == '\t') return false;
  if (value.back() == ' ' || value.back() == '\t') return false;
  for (const char ch : value) {
    const unsigned char u = static_cast<unsigned char>(ch);
    if (u == 0x09) continue;                       // tab
    if (u < 0x20 || u > 0x7E) return false;        // control or non-ASCII
  }
  return true;
}

std::string encodeHeaderValue(std::string_view raw) {
  if (isPlainHeaderValue(raw)) return std::string(raw);
  return std::string(kSentinelPrefix) + base64Encode(raw) + kSentinelSuffix;
}

bool decodeHeaderValue(std::string_view value, std::string& out) {
  const std::string_view prefix = kSentinelPrefix;
  const std::string_view suffix = kSentinelSuffix;
  if (value.size() > prefix.size() + suffix.size() && value.substr(0, prefix.size()) == prefix &&
      value.substr(value.size() - suffix.size()) == suffix) {
    return base64Decode(value.substr(prefix.size(), value.size() - prefix.size() - suffix.size()),
                        out);
  }
  out.assign(value);
  return true;
}

}  // namespace mcp::detail
