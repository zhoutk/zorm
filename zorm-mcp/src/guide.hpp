#pragma once
// guide.hpp - the AI-comprehension layer.
//
// How an AI client learns to drive this server, in order of effectiveness:
//   1. `instructions()` - injected into the model context at connect time
//      (MCP's designated field; Claude Code / VS Code / Cursor all surface it).
//   2. Per-tool descriptions + inputSchema property descriptions - always in
//      context; each carries "use me when / prefer X for Y" routing hints.
//   3. Error messages that teach (see data_tools.cpp / sql_tools.cpp) - the
//      cheapest self-correction loop an agent has.
//   4. Pull channels as fallback: the `db-playbook` prompt and the
//      `db://guide` resource (for clients without instructions support, and
//      for agents that go looking).
//
// Everything lives here so the wording stays coherent and auditable.

#include <mcp/mcp.hpp>

#include <string>

namespace zorm_mcp {

// Server-level instructions text (the connect-time decision table).
std::string instructionsText();

// The full playbook (prompt/resource payload; the long-form manual).
std::string playbookText();

// Registers the `db-playbook` prompt on a Builder.
void registerPlaybookPrompt(mcp::server::Builder& b);

// Resource handler for `db://guide` (registered by resources.cpp).
mcp::McpResult<mcp::Json> resourceGuide(mcp::server::Ctx& c);

}  // namespace zorm_mcp
