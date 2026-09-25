#pragma once
// tools.hpp - registration entry points and the shared MRTR staging machinery
// declaration for the three tool TUs.

#include <mcp/mcp.hpp>

#include <string>

namespace mcp {
namespace server {
struct Ctx;
class Builder;
}  // namespace server
}  // namespace mcp

namespace zorm_mcp {

// meta_tools.cpp - read-only schema/metadata tools.
void registerMetaTools(mcp::server::Builder& b);
// data_tools.cpp - smart query + CRUD write tools (MRTR).
void registerDataTools(mcp::server::Builder& b);
// sql_tools.cpp - SQL escape hatches (query_sql always; exec_sql/trans gated).
void registerSqlTools(mcp::server::Builder& b);
// resources.cpp - db://schema + db://guide resources and completion.
void registerResourcesAndCompletion(mcp::server::Builder& b);

// Stages `kind` for MRTR confirmation (or executes immediately with --yes /
// on an approved retry). data_tools.cpp holds the shared implementation;
// sql_tools.cpp reuses it for db_exec_sql / db_trans.
mcp::McpResult<mcp::Json> stageWrite(mcp::server::Ctx& c, const std::string& kind);

// Executes an already-staged write (MRTR-approved retry / --yes fast path).
mcp::McpResult<mcp::Json> executeWrite(const std::string& dbName,
                                       const std::string& table,
                                       const std::string& kind,
                                       const mcp::Json& args);

// Builds the input_required result asking the user to confirm a write.
mcp::Json inputRequiredConfirm(const std::string& requestState,
                               const std::string& message);

}  // namespace zorm_mcp
