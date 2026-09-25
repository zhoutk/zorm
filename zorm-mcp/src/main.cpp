// zorm-mcp - a complete, production-grade database-operation MCP server over
// zorm (unified multi-backend ORM) + mcp-cpp (C++ MCP SDK).
//
// Tools (all take a `db` routing parameter; zorm's dialect layer absorbs the
// SQL differences, so the tool semantics are identical on every backend):
//   metadata  db_list_tables / db_describe_table / db_indexes / db_info
//   query     db_query (structured smart query) / db_count / db_query_sql
//   writes    db_insert / db_insert_batch / db_update / db_delete / db_trans
//             (all MRTR-confirmed) / db_exec_sql (--allow-sql, MRTR)
//   extras    db://schema resources, db://guide, db-playbook prompt, completion
//
// Modes:
//   (default)   stdio MCP server (register in Claude Code / VS Code / ...)
//   --http N    Streamable HTTP on 127.0.0.1:N instead of stdio
//   --yes       disable MRTR write confirmation (automation/tests only)
//   --allow-sql expose db_exec_sql (dangerous; default hidden)
//   --config F  JSON config with a "databases" object (see README.md)
//   --child     internal: server mode for the e2e test's spawned child

#include "app.hpp"
#include "guide.hpp"
#include "json_util.hpp"
#include "tools.hpp"
#include <mcp/transport_http.hpp>

#include <chrono>
#include <cstdio>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

using namespace mcp;
using namespace mcp::server;

namespace {

// Builds the MCP server with the full tool/resource/prompt surface.
std::shared_ptr<Server> buildServer() {
  zorm_mcp::App& g = zorm_mcp::app();
  Builder b;
  b.acceptLegacyClients(true);  // Claude Code / VS Code speak legacy initialize
  // Tools are version-agnostic; speak every protocol revision so legacy
  // clients (initialize with older dates) negotiate successfully.
  b.supports(ProtocolVersion::V2024_11_05)
      .supports(ProtocolVersion::V2025_03_26)
      .supports(ProtocolVersion::V2025_06_18)
      .supports(ProtocolVersion::V2025_11_25)
      .supports(ProtocolVersion::Latest);

  b.name("zorm-mcp").version("1.0.0").instructions(zorm_mcp::instructionsText());

  zorm_mcp::registerMetaTools(b);
  zorm_mcp::registerDataTools(b);
  zorm_mcp::registerSqlTools(b);
  if (g.allowSql) {
    // Raw write/DDL SQL: operator opt-in, MRTR-confirmed when enabled.
    b.tool(Json(R"({"name":"db_exec_sql","description":"Execute ARBITRARY write/DDL SQL (INSERT/UPDATE/DELETE/CREATE/...), MRTR-confirmed. Use only for what the structured tools cannot express (schema changes, bulk conditional ops). Prefer db_query_sql for reads and db_trans for atomic batches. Bind values via values array (? placeholders) rather than concatenating them into sql.","inputSchema":{"type":"object","required":["db","sql"],"properties":{"db":{"type":"string","description":"backend name"},"sql":{"type":"string"},"values":{"type":"array","description":"positional bindings for ? placeholders"},"requestState":{"type":"string"},"inputResponses":{"type":"object"}}}})"),
           [](Ctx& c) { return zorm_mcp::stageWrite(c, "exec_sql"); });
  }
  zorm_mcp::registerResourcesAndCompletion(b);
  zorm_mcp::registerPlaybookPrompt(b);

  auto server = b.build();
  g.server = server;
  return server;
}

}  // namespace

int main(int argc, char** argv) {
  zorm_mcp::App& g = zorm_mcp::app();
  bool doHttp = false;
  int httpPort = 8080;
  std::string configPath;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--http") {
      doHttp = true;
      if (i + 1 < argc) httpPort = std::atoi(argv[++i]);
    } else if (a == "--yes") {
      g.confirmWrites = false;
    } else if (a == "--allow-sql") {
      g.allowSql = true;
    } else if (a == "--config" && i + 1 < argc) {
      configPath = argv[++i];
    } else if (a == "--child") {
      // internal: spawned by the e2e test
    } else if (a == "--help" || a == "-h") {
      std::cout << "zorm-mcp - multi-backend database MCP server (zorm + mcp-cpp)\n"
                   "usage: zorm_mcp_server [--config <file>] [--http <port>] [--yes]\n"
                   "                       [--allow-sql] [--child]\n"
                   "default mode is stdio; see README.md for client registration\n";
      return 0;
    } else {
      std::cerr << "usage: zorm_mcp_server [--config <file>] [--http <port>] "
                   "[--yes] [--allow-sql] [--child]\n";
      return 2;
    }
  }

  if (!zorm_mcp::loadSpecs(configPath, doHttp)) return 2;
  if (!zorm_mcp::openBackends()) return 1;
  zorm_mcp::discoverAllSchemas();

  auto server = buildServer();

  // --http: HTTP-ONLY serving (stdio not run).
  std::unique_ptr<mcp::http::HttpServerTransport> http;
  if (doHttp) {
    try {
      http = std::make_unique<mcp::http::HttpServerTransport>(*server);
      if (http->runAsync("127.0.0.1", httpPort).has_value()) {
        std::cerr << "[zorm-mcp] HTTP serving on 127.0.0.1:" << httpPort << "/mcp"
                  << std::endl;
      } else {
        std::cerr << "[zorm-mcp] HTTP listen failed" << std::endl;
        http.reset();
      }
    } catch (...) {
      std::cerr << "[zorm-mcp] HTTP transport unavailable (built without httplib?)"
                << std::endl;
    }
    // Deterministic: --http means HTTP-ONLY serving (the stdio loop would
    // exit on immediate EOF when stdin is /dev/null, killing the process).
    if (http) {
      while (http->running()) std::this_thread::sleep_for(std::chrono::milliseconds(200));
      return 0;
    }
  }

  // Default: stdio MCP server.
  StdioTransport transport;
  const McpResult<void> r = server->run(transport);
  if (!r.has_value()) {
    std::cerr << "[zorm-mcp] transport error: " << r.error().message << std::endl;
    return 1;
  }
  return 0;
}
