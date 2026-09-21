#pragma once
// ---------------------------------------------------------------------------
// mcp/version.hpp - MCP protocol version model.
//
// The wire format carries the version as a date string ("2026-07-28").
// The enum keeps the value small and comparable; toString/fromString are the
// single source of truth for the wire spelling of each version.
// ---------------------------------------------------------------------------

#include <cstdint>
#include <optional>
#include <string_view>

namespace mcp {

// All protocol versions the SDK can speak, oldest first.
// (Verified against the 2026-07-28 specification versioning history; the
// plan's "6 versions" phrasing was a typo - there are five.)
enum class ProtocolVersion : uint8_t {
  V2024_11_05,
  V2025_03_26,
  V2025_06_18,
  V2025_11_25,
  V2026_07_28,
  Latest = V2026_07_28,
  Oldest = V2024_11_05,
};

inline constexpr std::size_t kProtocolVersionCount = 5;

// Wire-format date string for a version ("" for an invalid value).
std::string_view toString(ProtocolVersion v);

// Parses a wire-format version string; nullopt when unknown.
std::optional<ProtocolVersion> fromString(std::string_view s);

// SDK release version (keep in sync with version.txt).
inline constexpr std::string_view kSdkVersion = "0.1.0";

}  // namespace mcp
