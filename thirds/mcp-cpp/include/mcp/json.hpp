#pragma once
// ---------------------------------------------------------------------------
// mcp/json.hpp - JSON abstraction entry point.
//
// M0 decision (PLAN.md Update 3): mcp::Json is a direct alias of the bundled
// zjson `Json` (arena-backed, zero-copy views, exact int64/uint64/double
// three-state numbers - required for protocol int64 fidelity). The planned
// `IJsonAdapter` injection point lands in M1 together with the message
// model, once the minimal adapter surface is known; the alias keeps all
// call sites written against `mcp::Json` in the meantime.
// ---------------------------------------------------------------------------

#include <zjson.hpp>

namespace mcp {

using Json = ZJSON::Json;

}  // namespace mcp
