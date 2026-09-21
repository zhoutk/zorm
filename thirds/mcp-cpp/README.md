# mcp-cpp

Independent MCP (Model Context Protocol) C++ SDK — C++17, no dependencies beyond a bundled JSON single-header (zjson) and optionally cpp-httplib for HTTP. Targets the MCP specification **2026-07-28** with Tier-1 SDK (TS/Python/C#/Go/Rust) feature coverage and a value-semantic C++ design (no async runtime required).

**Status: M0–M5 complete (feature-complete per PLAN.md §5 matrix).** Roadmap and decision log: [PLAN.md](PLAN.md).

## Feature matrix (vs the official Tier-1 SDKs)

| Feature | mcp-cpp | Tier-1 parity |
| --- | --- | --- |
| JSON-RPC 2.0 strict codec (MCP deltas: null-id ban, no batching, object params) | ✅ | ✅ |
| Protocol versions 2024-11-05 … 2026-07-28 + `-32022` negotiation | ✅ | ✅ |
| `server/discover` (supportedVersions/capabilities/ttl/cacheScope/serverInfo) | ✅ | ✅ |
| Stateful-free per-request `_meta` (protocolVersion/clientCapabilities/clientInfo) | ✅ | ✅ |
| stdio transport (newline framing, binary mode on Windows, EOF shutdown) | ✅ | ✅ |
| Streamable HTTP (POST-only, header mirroring, `Mcp-Param-*`, 202/404/405, `-32020`) | ✅ | ✅ |
| SSE response streams + `subscriptions/listen` (ack, filter fan-out, graceful closure, keep-alive) | ✅ | ✅ |
| tools/resources/prompts registries, capability gating, pagination cursors | ✅ | ✅ |
| `notifications/cancelled` + queryable cancellation flag | ✅ | ✅ |
| MRTR: `input_required` results + `inputResponses` retry | ✅ | ✅ |
| `completion/complete` | ✅ | ✅ |
| CacheableResult (`ttlMs`/`cacheScope`) on discover + lists | ✅ | ✅ |
| JSON Schema 2020-12 validator (local `$ref`, resource bounds, no remote refs) | ✅ | TS: ajv / py: pydantic / go: gojsonschema / rust: jsonschema |
| `x-mcp-header` mirroring (client) + server-side validation | ✅ | ✅ |
| Roots / Sampling (deprecated by SEP-2577) | ❌ intentionally | Tier-1 keeps compat shims |
| Tasks extension (`io.modelcontextprotocol/tasks`) | ⏳ v2 | extension |
| OAuth 2.1 / Client ID Metadata | ⏳ v2 | ✅ |

**C++17-only build** (`std::expected` is aliased automatically when the standard library provides it). Value-semantic message model, exception-free protocol layer, `[[nodiscard]]` result types with `Ok/Err` factories that read the same under both result implementations.

## Benchmarks

`RelWithDebInfo`, standalone build, clang 22.1.8 (MSYS2 clang64, windows-gnu), same machine and message shapes for both:

| Bench | mcp-cpp | serde_json (rust-sdk's parser) |
| --- | --- | --- |
| B1 parse ~1KB request | 714,000 msg/s (1.40 µs) | 907,000 msg/s (1.10 µs) |
| B1 parse ~64KB request | 35,900 msg/s (27.8 µs) | 136,800 msg/s (7.31 µs) |
| B2 tools/call dispatch round trip (gates + schema validation + encode) | 298,500 msg/s (3.35 µs) | n/a (needs full tokio stack) |
| B3 stdio end-to-end round trip (real pipes, child server) | 24,700 msg/s (40.4 µs) | n/a |

Honest reading (PLAN.md Update 8): raw JSON parse does not beat serde_json — zjson's copy-on-parse model plus our strict semantic validation costs more than serde's zero-copy borrow parse, especially at 64KB. The stated optimization paths: zero-copy `string_view` parse (zjson arena already supports borrowed views), envelope pre-scan fast path. The B2/B3 numbers are full-stack (validation included) and are the ones applications actually experience.

## Build & test

Standalone (needs a zjson copy under `thirds/zjson`, or pass `-DMCP_ZJSON_DIR=<dir>`):

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build
ctest --test-dir build --output-on-failure
```

Options: `MCP_BUILD_TESTS` (default ON), `MCP_DISABLE_HTTP` (pure-stdio build), `MCP_ZJSON_DIR`.

Inside the zorm repo the library is picked up by the root CMakeLists (googletest and zjson are reused); run tests with `ctest -R mcp_test`.

## Using it

```cpp
#include <mcp/mcp.hpp>

mcp::server::Builder b;
b.name("my-server").version("1.0");
b.simpleTool("add", "adds two integers", [](mcp::server::Ctx& c) -> mcp::McpResult<mcp::Json> {
  mcp::Json result;
  result.add("sum", mcp::Json(c.params["arguments"]["a"].toInt64() +
                              c.params["arguments"]["b"].toInt64()));
  return mcp::Ok(std::move(result));
});
auto server = b.build();
mcp::StdioTransport transport;
(void)server->run(transport);
```

Client side mirrors it: `mcp::client::Client` over any `ITransport` (stdio, in-memory, or `mcp::http::HttpClientTransport`).

### zorm integration

`examples/zorm_server.cpp` (built only inside the zorm repo) exposes a live zorm database as four MCP tools (`db_select/db_insert/db_update/db_delete`) with JSON-Schema-validated arguments — a database as an MCP server in ~150 lines.

## Layout

```
include/mcp/   public headers (mcp.hpp aggregates everything)
src/           compiled implementation
tests/         googletest unit tests (10 CTest registrations, incl. real
               subprocess stdio e2e and loopback HTTP/SSE e2e)
bench/         B1 parse / B2 dispatch / B3 stdio e2e benchmarks
examples/      minimal_server (Inspector-ready) + zorm_server
```

## License

MIT — see [LICENSE](LICENSE) and [NOTICE](NOTICE) for bundled third-party components (zjson, cpp-httplib, googletest).
