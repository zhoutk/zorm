#include <mcp/version.hpp>

#include <array>

namespace mcp {
namespace {

struct VersionName {
  ProtocolVersion version;
  std::string_view name;
};

constexpr std::array<VersionName, kProtocolVersionCount> kVersions{{
    {ProtocolVersion::V2024_11_05, "2024-11-05"},
    {ProtocolVersion::V2025_03_26, "2025-03-26"},
    {ProtocolVersion::V2025_06_18, "2025-06-18"},
    {ProtocolVersion::V2025_11_25, "2025-11-25"},
    {ProtocolVersion::V2026_07_28, "2026-07-28"},
}};

static_assert(kVersions.front().version == ProtocolVersion::Oldest &&
                  kVersions.back().version == ProtocolVersion::Latest,
              "version table must be ordered oldest-first");

}  // namespace

std::string_view toString(ProtocolVersion v) {
  for (const auto& entry : kVersions) {
    if (entry.version == v) return entry.name;
  }
  return {};
}

std::optional<ProtocolVersion> fromString(std::string_view s) {
  for (const auto& entry : kVersions) {
    if (entry.name == s) return entry.version;
  }
  return std::nullopt;
}

}  // namespace mcp
