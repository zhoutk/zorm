// B1 - parse throughput: decode-only cost per message size.
// Baseline comparison (PLAN.md §8): rust-sdk pays serde_json decode + typed
// struct conversion; we measure the full parseMessage path.
//
// Usage: mcp_bench_parse [iterations]

#include <mcp/mcp.hpp>

#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

using namespace mcp;

namespace {

std::string makeRequestLine(size_t argsBytes, const RequestId& id) {
  // Deterministic filler: args string of ~argsBytes.
  std::string filler(argsBytes, 'x');
  Json arguments;
  arguments.add("text", Json::str(filler));
  Json params = encodeRequestMeta("2026-07-28", Json(R"({})"), std::nullopt);
  params.add("name", Json::str("echo"));
  params.add("arguments", std::move(arguments));
  return encodeRequest(id, "tools/call", params);
}

double bench(const std::vector<std::string>& lines, int iterations) {
  const auto start = std::chrono::steady_clock::now();
  uint64_t sink = 0;
  for (int i = 0; i < iterations; ++i) {
    for (const auto& line : lines) {
      const ParsedMessage parsed = parseMessage(line);
      if (const auto* r = std::get_if<JsonRpcRequest>(&parsed)) {
        sink += static_cast<uint64_t>(r->id.index());  // defeat optimization
      }
    }
  }
  const auto end = std::chrono::steady_clock::now();
  if (sink == 1) std::fputs("", stdout);
  const double secs = std::chrono::duration<double>(end - start).count();
  return static_cast<double>(lines.size()) * iterations / secs;
}

}  // namespace

int main(int argc, char** argv) {
  const int iterations = (argc > 1) ? std::atoi(argv[1]) : 20000;

  const RequestId id(std::string("bench"));
  const std::vector<std::string> small{makeRequestLine(16, id)};        // ~1KB line
  const std::vector<std::string> large{makeRequestLine(64 * 1024, id)};  // ~64KB line

  const double smallOps = bench(small, iterations);
  const double largeOps = bench(large, iterations / 8);

  std::printf("B1 parse\n");
  std::printf("  small (~1KB):  %10.0f msg/s  (%.2f us/msg)\n", smallOps, 1e6 / smallOps);
  std::printf("  large (~64KB): %10.0f msg/s  (%.2f us/msg)\n", largeOps, 1e6 / largeOps);
  return 0;
}
