// B2 - dispatch: full server round trip (parse -> meta/version/capability
// gates -> handler -> encode) for tools/call over an in-memory transport.
// Baseline comparison (PLAN.md §8): rust-sdk single-task tokio dispatch.

#include <mcp/mcp.hpp>

#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

using namespace mcp;
using namespace mcp::server;

namespace {

// Capture sink: counts writes, discards content.
class NullTransport final : public ITransport {
 public:
  McpResult<std::optional<std::string>> read() override {
    return Ok(std::optional<std::string>(std::nullopt));
  }
  McpResult<void> write(std::string_view) override {
    writes++;
    return Ok();
  }
  McpResult<void> flush() override { return Ok(); }
  uint64_t writes = 0;
};

}  // namespace

int main(int argc, char** argv) {
  const int iterations = (argc > 1) ? std::atoi(argv[1]) : 50000;

  Builder b;
  b.name("bench").version("1.0");
  b.simpleTool("echo", "echoes", [](Ctx& c) -> McpResult<Json> {
    Json result;
    result.add("echoed", Json::str(c.params["arguments"]["text"].toString()));
    return Ok(std::move(result));
  });
  auto server = b.build();

  Json arguments;
  arguments.add("text", Json::str("payload-16b"));
  Json params = encodeRequestMeta("2026-07-28", Json(R"({})"), std::nullopt);
  params.add("name", Json::str("echo"));
  params.add("arguments", std::move(arguments));
  const std::string line = encodeRequest(RequestId(int64_t(1)), "tools/call", params);

  NullTransport sink;
  // warmup
  for (int i = 0; i < 1000; ++i) server->processLine(line, sink);

  const auto start = std::chrono::steady_clock::now();
  for (int i = 0; i < iterations; ++i) server->processLine(line, sink);
  const auto end = std::chrono::steady_clock::now();

  const double secs = std::chrono::duration<double>(end - start).count();
  std::printf("B2 dispatch\n");
  std::printf("  tools/call round trip: %10.0f msg/s  (%.3f us/msg)  writes=%llu\n",
              iterations / secs, 1e6 * secs / iterations, static_cast<unsigned long long>(sink.writes));
  return 0;
}
