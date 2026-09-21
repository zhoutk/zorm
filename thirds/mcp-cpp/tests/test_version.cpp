// Protocol version model: wire spellings, roundtrip, unknown handling.

#include <mcp/mcp.hpp>

#include <gtest/gtest.h>

#include <array>

using namespace mcp;

TEST(Version, WireStrings) {
  EXPECT_EQ(toString(ProtocolVersion::V2024_11_05), "2024-11-05");
  EXPECT_EQ(toString(ProtocolVersion::V2025_03_26), "2025-03-26");
  EXPECT_EQ(toString(ProtocolVersion::V2025_06_18), "2025-06-18");
  EXPECT_EQ(toString(ProtocolVersion::V2025_11_25), "2025-11-25");
  EXPECT_EQ(toString(ProtocolVersion::V2026_07_28), "2026-07-28");
}

TEST(Version, Roundtrip) {
  for (auto v : std::array<ProtocolVersion, kProtocolVersionCount>{
           ProtocolVersion::V2024_11_05, ProtocolVersion::V2025_03_26,
           ProtocolVersion::V2025_06_18, ProtocolVersion::V2025_11_25,
           ProtocolVersion::V2026_07_28}) {
    auto parsed = fromString(toString(v));
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(*parsed, v);
  }
}

TEST(Version, LatestAndOldestAliases) {
  EXPECT_EQ(toString(ProtocolVersion::Latest), "2026-07-28");
  EXPECT_EQ(toString(ProtocolVersion::Oldest), "2024-11-05");
  EXPECT_EQ(ProtocolVersion::Latest, ProtocolVersion::V2026_07_28);
}

TEST(Version, UnknownStrings) {
  EXPECT_FALSE(fromString("").has_value());
  EXPECT_FALSE(fromString("2025-01-01").has_value());
  EXPECT_FALSE(fromString("2026-7-28").has_value());  // zero padding matters
  EXPECT_FALSE(fromString("2024-11-05x").has_value());
  EXPECT_FALSE(fromString(" 2024-11-05").has_value());
}

TEST(Version, SdkVersionMacro) {
  EXPECT_FALSE(kSdkVersion.empty());
}
