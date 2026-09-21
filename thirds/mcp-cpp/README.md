# mcp-cpp

Independent MCP (Model Context Protocol) C++ SDK — C++17, no dependencies beyond a bundled JSON single-header (zjson). Targets the MCP specification **2026-07-28** and aims at Tier-1 SDK feature parity (TS/Python/C#/Go/Rust) with a value-semantic C++ design.

**Status: M0 (skeleton).** The roadmap, design decisions and decision log live in [PLAN.md](PLAN.md). New surfaces land milestone by milestone — M1 protocol core (types + JSON-RPC), M2 server builder, M3 client + HTTP, M4 schema validation, M5 benchmarks/release.

## What exists now

- `mcp::ProtocolVersion` — the five protocol versions with wire-string conversions (`mcp/version.hpp`)
- `mcp::McpErrorCode` / `mcp::McpError` — JSON-RPC standard codes plus the MCP-reserved range `-32020..-32099`, values pinned to the spec (`mcp/error.hpp`)
- `mcp::McpResult<T>` — `std::expected<T, McpError>` when the standard library has it (C++23), otherwise a built-in value-semantic fallback with the same member surface; error returns via `Ok(...)` / `Err(...)` factories
- `mcp::Json` — alias of the bundled zjson `Json` (arena-backed, exact int64/uint64/double numbers)

## Build & test

Standalone (needs a zjson copy under `thirds/zjson`, or pass `-DMCP_ZJSON_DIR=<dir>`):

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build
ctest --test-dir build --output-on-failure
```

Inside the zorm repo the library is picked up by the root CMakeLists (googletest and zjson are reused from the repo); run the tests with `ctest -R mcp_test` from the repo build directory.

## Layout

```
include/mcp/   public headers (mcp.hpp is the aggregate entry point)
src/           compiled implementation
tests/         googletest unit tests (registered with CTest)
bench/         performance baselines (M5)
examples/      zorm server + minimal server (M2+)
```
