// zorm-mcp - end-to-end test.
//
// Spawns the zorm_mcp_server executable as a child (real stdio pipes) and
// drives it with the mcp-cpp Client API - the exact path an AI client takes.
// Coverage goal: if this suite is green, a real agent client can basically
// use the server correctly. That means asserting not just tool outcomes but
// the WIRE SHAPES AI clients validate:
//   - every tools/call result carries the mandatory content array
//   - input_required results carry requestState + inputRequests.confirm
//   - errors are JSON-RPC InvalidParams with teaching messages
//
// The child runs with --child over the open-box defaults (sqlite3 + jsonfile
// in the exe's working directory); both scratch tables are cleaned at the
// start and end of each run so the suite is idempotent.

#include <mcp/mcp.hpp>

#include <gtest/gtest.h>

#include <cstdio>
#include <optional>
#include <string>
#include <vector>

using namespace mcp;
using namespace mcp::server;
using namespace mcp::client;

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>

namespace {

struct Child {
  HANDLE inWrite = nullptr, outRead = nullptr;
  PROCESS_INFORMATION pi{};
  ~Child() {
    if (inWrite) CloseHandle(inWrite);
    if (outRead) CloseHandle(outRead);
    if (pi.hProcess) {
      // Graceful wait, then hard-kill: a stuck child must never leak into the
      // terminal (a leaked child once froze the shell it was spawned from).
      if (WaitForSingleObject(pi.hProcess, 5000) != WAIT_OBJECT_0) {
        TerminateProcess(pi.hProcess, 1);
        WaitForSingleObject(pi.hProcess, 2000);
      }
      CloseHandle(pi.hProcess);
      CloseHandle(pi.hThread);
    }
  }
  bool writeLine(const std::string& line) const {
    const std::string buf = line + "\n";
    DWORD written = 0;
    return WriteFile(inWrite, buf.data(), static_cast<DWORD>(buf.size()), &written, nullptr) &&
           written == buf.size();
  }
  bool readLine(std::string& out) const {
    out.clear();
    char ch = 0;
    DWORD got = 0;
    while (ReadFile(outRead, &ch, 1, &got, nullptr) && got == 1) {
      if (ch == '\n') return true;
      if (ch != '\r') out.push_back(ch);
    }
    return false;
  }
  void closeStdin() {
    if (inWrite) {
      CloseHandle(inWrite);
      inWrite = nullptr;
    }
  }
};

Child spawnChild(bool confirmWrites, bool allowSql) {
#ifdef ZORM_DLL_DIR
  // The server child loads zorm's runtime DLLs; make sure they resolve even
  // when ctest runs outside the run-test PATH setup.
  char pathBuf[8192] = {0};
  GetEnvironmentVariableA("PATH", pathBuf, sizeof(pathBuf));
  std::string newPath = ZORM_DLL_DIR ";" + std::string(pathBuf);
  SetEnvironmentVariableA("PATH", newPath.c_str());
#endif
  // Spawn the REAL server (sibling of this test executable) - NOT this test
  // itself: the test's main is gtest_main with no --child handler, so
  // self-spawning would recursively run the whole suite (fork bomb).
  char testPath[MAX_PATH];
  GetModuleFileNameA(nullptr, testPath, MAX_PATH);
  std::string serverPath(testPath);
  const size_t slash = serverPath.find_last_of("\\/");
  serverPath = serverPath.substr(0, slash + 1) + "zorm_mcp_server.exe";

  std::string cmdline = std::string("\"") + serverPath + "\" --child";
  if (!confirmWrites) cmdline += " --yes";
  if (allowSql) cmdline += " --allow-sql";
  SECURITY_ATTRIBUTES sa{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
  HANDLE inRead = nullptr, outWrite = nullptr;
  Child c;
  EXPECT_TRUE(CreatePipe(&inRead, &c.inWrite, &sa, 0));
  EXPECT_TRUE(CreatePipe(&c.outRead, &outWrite, &sa, 0));
  if (c.inWrite) SetHandleInformation(c.inWrite, HANDLE_FLAG_INHERIT, 0);
  if (c.outRead) SetHandleInformation(c.outRead, HANDLE_FLAG_INHERIT, 0);
  STARTUPINFOA si{};
  si.cb = sizeof(si);
  si.hStdInput = inRead;
  si.hStdOutput = outWrite;
  si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
  si.dwFlags |= STARTF_USESTDHANDLES;
  EXPECT_TRUE(CreateProcessA(nullptr, cmdline.data(), nullptr, nullptr, TRUE, 0, nullptr,
                             nullptr, &si, &c.pi));
  CloseHandle(inRead);
  CloseHandle(outWrite);
  return c;
}

}  // namespace
#else
#include <sys/wait.h>
#include <unistd.h>
namespace {
struct Child {
  int inWrite = -1, outRead = -1;
  pid_t pid = -1;
  ~Child() {
    if (inWrite >= 0) close(inWrite);
    if (outRead >= 0) close(outRead);
    if (pid > 0) waitpid(pid, nullptr, 0);
  }
  bool writeLine(const std::string& line) const {
    const std::string buf = line + "\n";
    return ::write(inWrite, buf.data(), buf.size()) == static_cast<ssize_t>(buf.size());
  }
  bool readLine(std::string& out) const {
    out.clear();
    char ch = 0;
    while (::read(outRead, &ch, 1) == 1) {
      if (ch == '\n') return true;
      if (ch != '\r') out.push_back(ch);
    }
    return false;
  }
  void closeStdin() {
    if (inWrite >= 0) {
      close(inWrite);
      inWrite = -1;
    }
  }
};
Child spawnChild(bool confirmWrites, bool allowSql) {
  // Spawn the real server executable sitting next to this test (never
  // self-spawn: this test's main is gtest_main without --child).
  std::string self = "/proc/self/exe";
  char buf[4096] = {0};
  ssize_t n = readlink(self.c_str(), buf, sizeof(buf) - 1);
  std::string serverPath = (n > 0) ? std::string(buf, static_cast<size_t>(n)) : self;
  const size_t slash = serverPath.find_last_of('/');
  serverPath = serverPath.substr(0, slash + 1) + "zorm_mcp_server";

  int inPipe[2], outPipe[2];
  pipe(inPipe); pipe(outPipe);
  Child c;
  c.pid = fork();
  if (c.pid == 0) {
    dup2(inPipe[0], STDIN_FILENO);
    dup2(outPipe[1], STDOUT_FILENO);
    close(inPipe[0]); close(inPipe[1]); close(outPipe[0]); close(outPipe[1]);
    std::vector<char*> childArgv = {const_cast<char*>("zorm_mcp_server"),
                                    const_cast<char*>("--child"), nullptr};
    std::string yesFlag, sqlFlag;
    if (!confirmWrites) {
      yesFlag = "--yes";
      childArgv.push_back(const_cast<char*>(yesFlag.c_str()));
    }
    if (allowSql) {
      sqlFlag = "--allow-sql";
      childArgv.push_back(const_cast<char*>(sqlFlag.c_str()));
    }
    execv(serverPath.c_str(), childArgv.data());
    _exit(127);
  }
  close(inPipe[0]); close(outPipe[1]);
  c.inWrite = inPipe[1]; c.outRead = outPipe[0];
  return c;
}
}  // namespace
#endif

namespace {

// A pipe-backed ITransport so the mcp-cpp Client drives the child directly.
struct PipeTransport final : ITransport {
  Child& child;
  explicit PipeTransport(Child& c) : child(c) {}
  McpResult<std::optional<std::string>> read() override {
    std::string line;
    if (!child.readLine(line)) return Ok(std::optional<std::string>(std::nullopt));
    return Ok(std::optional<std::string>(std::move(line)));
  }
  McpResult<void> write(std::string_view message) override {
    if (!child.writeLine(std::string(message)))
      return Err(McpErrorCode::InternalError, "write to child failed");
    return Ok();
  }
  McpResult<void> flush() override { return Ok(); }
};

const char* kE2eTable = "t_e2e";

// Builds {"db": dbName, ...extra} call arguments.
Json argsWith(const char* dbName) {
  Json a;
  a.add("db", Json::str(dbName));
  return a;
}

// Spec compliance: every tools/call result - success, input_required, or
// cancel - must carry a content array (Claude Code rejects results without).
void expectEnvelope(const Json& result) {
  ASSERT_TRUE(result.isObject());
  ASSERT_TRUE(result["content"].isArray()) << result.toString();
  ASSERT_GE(result["content"].size(), 1u);
  EXPECT_EQ(result["content"][0]["type"].toString(), "text");
}

// db_exec_sql convenience (child must run --allow-sql).
McpResult<Json> execSql(Client& client, const std::string& sql) {
  Json a = argsWith("sqlite3");
  a.add("sql", Json::str(sql));
  return client.callTool("db_exec_sql", a);
}

// Calls a write tool and, when the (possibly MRTR-confirming) server answers
// input_required, completes the confirmation with approved:true. This is how
// fixtures stay correct under BOTH --yes and confirmation-on children.
McpResult<Json> callWrite(Client& client, const char* tool, Json args) {
  auto rs = client.callTool(tool, args);
  if (rs.has_value() && (*rs)["resultType"].toString() == "input_required") {
    Json retry = args;
    retry.add("requestState", (*rs)["requestState"]);
    retry.add("inputResponses", Json(R"({"confirm":{"approved":true}})"));
    rs = client.callTool(tool, retry);
  }
  return rs;
}

// Creates the shared e2e table and empties it (idempotent across runs).
void resetTable(Client& client) {
  Json create = argsWith("sqlite3");
  create.add("sql", Json::str(std::string("create table if not exists ") + kE2eTable +
                              " (id varchar(32) primary key, name varchar(32), score double)"));
  auto rs = callWrite(client, "db_exec_sql", create);
  ASSERT_TRUE(rs.has_value()) << rs.error().message;
  ASSERT_EQ((*rs)["status"].toInt(), 200) << rs->toString();
  Json del = argsWith("sqlite3");
  del.add("sql", Json::str(std::string("delete from ") + kE2eTable));
  rs = callWrite(client, "db_exec_sql", del);
  ASSERT_TRUE(rs.has_value()) << rs.error().message;
  ASSERT_EQ((*rs)["status"].toInt(), 200) << rs->toString();
}

// Empties a jsonfile scratch table through the structured tools (the path a
// delete-all takes on the backend with no delete-all SQL support).
void clearJsonTable(Client& client, const char* table) {
  Json d = argsWith("jsonfile");
  d.add("table", Json::str(table));
  d.add("where", Json("{}"));
  auto rs = callWrite(client, "db_delete", d);
  ASSERT_TRUE(rs.has_value()) << rs.error().message;
  ASSERT_EQ((*rs)["status"].toInt(), 200) << rs->toString();
}

long long countOf(Client& client, const char* db, const char* table) {
  Json c = argsWith(db);
  c.add("table", Json::str(table));
  auto rs = client.callTool("db_count", c);
  EXPECT_TRUE(rs.has_value());
  if (!rs.has_value()) return -1;
  const Json data = member(*rs, "data");
  return (data.isArray() && data.size() > 0) ? data[0]["total"].toInt64() : -1;
}

bool hasTool(const Json& toolsArr, const char* name) {
  for (const auto& t : toolsArr.toVector()) {
    if (t["name"].toString() == name) return true;
  }
  return false;
}

}  // namespace

// ---- protocol surface -----------------------------------------------------------

TEST(ZormMcpE2E, DiscoverToolsListAndGating) {
  // --allow-sql OFF: db_exec_sql must be hidden from tools/list entirely.
  Child child = spawnChild(false, false);
  PipeTransport t(child);
  Client client("e2e", "1.0");
  client.connect(t);

  auto disc = client.discover();
  ASSERT_TRUE(disc.has_value());
  EXPECT_EQ((*disc)["resultType"].toString(), "complete");
  // The AI-comprehension layer: instructions must ride the discovery result.
  EXPECT_NE(member(*disc, "instructions").toString().find("db_describe_table"),
            std::string::npos);
  EXPECT_NE(member(*disc, "instructions").toString().find("Backends: {"),
            std::string::npos);

  auto tools = client.listTools();
  ASSERT_TRUE(tools.has_value());
  const Json toolsArr = member(*tools, "tools");
  ASSERT_TRUE(toolsArr.isArray());
  EXPECT_TRUE(hasTool(toolsArr, "db_list_tables"));
  EXPECT_TRUE(hasTool(toolsArr, "db_describe_table"));
  EXPECT_TRUE(hasTool(toolsArr, "db_indexes"));
  EXPECT_TRUE(hasTool(toolsArr, "db_info"));
  EXPECT_TRUE(hasTool(toolsArr, "db_query"));
  EXPECT_TRUE(hasTool(toolsArr, "db_count"));
  EXPECT_TRUE(hasTool(toolsArr, "db_query_sql"));
  EXPECT_TRUE(hasTool(toolsArr, "db_insert"));
  EXPECT_TRUE(hasTool(toolsArr, "db_insert_batch"));
  EXPECT_TRUE(hasTool(toolsArr, "db_update"));
  EXPECT_TRUE(hasTool(toolsArr, "db_delete"));
  EXPECT_TRUE(hasTool(toolsArr, "db_trans"));
  EXPECT_FALSE(hasTool(toolsArr, "db_exec_sql"));
  // Descriptions must teach routing (the AI reads these on every request).
  for (const auto& tool : toolsArr.toVector()) {
    EXPECT_FALSE(tool["description"].toString().empty()) << tool["name"].toString();
  }

  // Pull channels: playbook prompt + guide resource + schema resource.
  auto pg = client.request("prompts/get", Json(R"({"name":"db-playbook"})"));
  ASSERT_TRUE(pg.has_value());
  EXPECT_NE((*pg).toString().find("MRTR"), std::string::npos);
  auto guide = client.readResource("db://guide");
  ASSERT_TRUE(guide.has_value());
  EXPECT_NE((*guide).toString().find("db_describe_table"), std::string::npos);
  auto overview = client.readResource("db://schema");
  ASSERT_TRUE(overview.has_value());
  EXPECT_NE((*overview).toString().find("sqlite3"), std::string::npos);
  EXPECT_NE((*overview).toString().find("jsonfile"), std::string::npos);
}

// ---- metadata: the "get table fields" capability ---------------------------------

TEST(ZormMcpE2E, MetadataOnEmptyTable) {
  Child child = spawnChild(false, true);  // --yes --allow-sql
  PipeTransport t(child);
  Client client("e2e", "1.0");
  client.connect(t);
  ASSERT_NO_FATAL_FAILURE(resetTable(client));

  // db_describe_table on the EMPTY table - catalog metadata must report full
  // structure with zero rows present (this is what query-the-table cannot do).
  auto desc = client.callTool("db_describe_table",
                              Json(R"({"db":"sqlite3","table":"t_e2e"})"));
  ASSERT_TRUE(desc.has_value()) << desc.error().message;
  expectEnvelope(*desc);
  const Json cols = member(*desc, "columns");
  ASSERT_TRUE(cols.isArray());
  ASSERT_EQ(cols.size(), 3u);  // id / name / score
  EXPECT_EQ(cols[0]["name"].toString(), "id");
  EXPECT_TRUE(cols[0]["primary_key"].isTrue());
  EXPECT_NE(cols[1]["type"].toString(), "");          // name : varchar
  EXPECT_NE(cols[2]["type"].toString().find("double"), std::string::npos);
  EXPECT_TRUE(cols[1]["nullable"].isTrue());

  // Per-table resource reads the same metadata.
  auto res = client.readResource("db://schema/sqlite3/t_e2e");
  ASSERT_TRUE(res.has_value());
  EXPECT_NE((*res).toString().find("primary key"), std::string::npos);

  // db_list_tables + db_indexes + db_info.
  auto tables = client.callTool("db_list_tables", Json(R"({"db":"sqlite3"})"));
  ASSERT_TRUE(tables.has_value());
  EXPECT_NE((*tables).toString().find("t_e2e"), std::string::npos);
  auto idx = client.callTool("db_indexes",
                             Json(R"({"db":"sqlite3","table":"t_e2e"})"));
  ASSERT_TRUE(idx.has_value()) << idx.error().message;  // pk autoindex at least
  auto info = client.callTool("db_info", Json(R"({"db":"sqlite3"})"));
  ASSERT_TRUE(info.has_value());
  EXPECT_EQ((*info)["type"].toString(), "sqlite3");
  EXPECT_NE(member(*info, "server_version").toString(), "");

  // Negative: unknown db lists the available backends (self-correcting).
  auto badDb = client.callTool("db_list_tables", Json(R"({"db":"oracle"})"));
  ASSERT_FALSE(badDb.has_value());
  EXPECT_EQ(badDb.error().code, McpErrorCode::InvalidParams);
  EXPECT_NE(badDb.error().message.find("jsonfile"), std::string::npos);
  auto badTable = client.callTool("db_describe_table",
                                  Json(R"({"db":"sqlite3","table":"nope"})"));
  ASSERT_FALSE(badTable.has_value());
  EXPECT_EQ(badTable.error().code, McpErrorCode::InvalidParams);
}

// ---- the full data plane on sqlite ----------------------------------------------

TEST(ZormMcpE2E, DataPlaneEndToEnd) {
  Child child = spawnChild(false, true);  // --yes --allow-sql
  PipeTransport t(child);
  Client client("e2e", "1.0");
  client.connect(t);
  ASSERT_NO_FATAL_FAILURE(resetTable(client));
  const char* tb = "t_e2e";

  // db_insert: one row with explicit id (status 200 + content envelope).
  Json ins = argsWith("sqlite3");
  ins.add("table", Json::str(tb));
  ins.add("data", Json(R"({"id":"r1","name":"ada","score":95.5})"));
  auto rs = client.callTool("db_insert", ins);
  ASSERT_TRUE(rs.has_value()) << rs.error().message;
  expectEnvelope(*rs);
  EXPECT_EQ((*rs)["status"].toInt(), 200);

  // db_query: compare-prefix condition.
  auto qr = client.callTool(
      "db_query", Json(R"({"db":"sqlite3","table":"t_e2e","score":">,50"})"));
  ASSERT_TRUE(qr.has_value());
  EXPECT_NE((*qr).toString().find("ada"), std::string::npos);

  // db_query: fields projection + ins (IN list).
  auto pr = client.callTool(
      "db_query",
      Json(R"({"db":"sqlite3","table":"t_e2e","fields":["name"],"ins":{"id":["r1","r2"]}})"));
  ASSERT_TRUE(pr.has_value());
  const std::string projText = (*pr).toString();
  EXPECT_NE(projText.find("name"), std::string::npos);

  // db_query: lks fuzzy + sort + page/size pagination envelope.
  Json fq = argsWith("sqlite3");
  fq.add("table", Json::str(tb));
  fq.add("lks", Json(R"({"name":"%a%"})"));
  fq.add("sort", Json(R"(["score"])"));
  fq.add("page", Json(1));
  fq.add("size", Json(2));
  auto fr = client.callTool("db_query", fq);
  ASSERT_TRUE(fr.has_value()) << fr.error().message;

  // db_query: group + sum aggregation.
  Json gq = argsWith("sqlite3");
  gq.add("table", Json::str(tb));
  gq.add("group", Json(R"(["name"])"));
  gq.add("sum", Json(R"({"score":"total"})"));
  auto gr = client.callTool("db_query", gq);
  ASSERT_TRUE(gr.has_value()) << gr.error().message;
  EXPECT_NE((*gr).toString().find("total"), std::string::npos);

  // db_count with where.
  Json cw = argsWith("sqlite3");
  cw.add("table", Json::str(tb));
  cw.add("where", Json(R"({"name":"ada"})"));
  auto cnt = client.callTool("db_count", cw);
  ASSERT_TRUE(cnt.has_value());
  const Json cntData = member(*cnt, "data");
  ASSERT_TRUE(cntData.isArray() && cntData.size() > 0);
  EXPECT_EQ(cntData[0]["total"].toInt64(), 1);

  // db_update (single row by id).
  Json up = argsWith("sqlite3");
  up.add("table", Json::str(tb));
  up.add("data", Json(R"({"score":88})"));
  up.add("where", Json(R"({"id":"r1"})"));
  rs = client.callTool("db_update", up);
  ASSERT_TRUE(rs.has_value()) << rs.error().message;
  EXPECT_EQ((*rs)["status"].toInt(), 200);

  // db_update rejects a where without id (with guidance, not silence).
  Json badUp = argsWith("sqlite3");
  badUp.add("table", Json::str(tb));
  badUp.add("data", Json(R"({"score":1})"));
  badUp.add("where", Json(R"({"name":"ada"})"));
  rs = client.callTool("db_update", badUp);
  ASSERT_FALSE(rs.has_value());
  EXPECT_EQ(rs.error().code, McpErrorCode::InvalidParams);
  EXPECT_NE(rs.error().message.find("db_trans"), std::string::npos);

  // db_query_sql: parameterized read with values binding.
  Json sel = argsWith("sqlite3");
  sel.add("sql", Json::str("select name from t_e2e where id = ?"));
  sel.add("values", Json(R"(["r1"])"));
  rs = client.callTool("db_query_sql", sel);
  ASSERT_TRUE(rs.has_value()) << rs.error().message;
  EXPECT_NE((*rs).toString().find("ada"), std::string::npos);

  // db_query_sql: read-only gate rejects writes and statement smuggling.
  rs = client.callTool("db_query_sql",
                       Json(R"({"db":"sqlite3","sql":"delete from t_e2e"})"));
  ASSERT_FALSE(rs.has_value());
  EXPECT_EQ(rs.error().code, McpErrorCode::InvalidParams);
  EXPECT_NE(rs.error().message.find("READ-ONLY"), std::string::npos);
  rs = client.callTool(
      "db_query_sql", Json(R"({"db":"sqlite3","sql":"select 1; delete from t_e2e"})"));
  ASSERT_FALSE(rs.has_value());

  // db_insert_batch: multi-row upsert (r1 overwritten, r2 inserted).
  Json batch = argsWith("sqlite3");
  batch.add("table", Json::str(tb));
  batch.add("rows",
            Json(R"([{"id":"r1","name":"ada2","score":10},{"id":"r2","name":"bob","score":20}])"));
  rs = client.callTool("db_insert_batch", batch);
  ASSERT_TRUE(rs.has_value()) << rs.error().message;
  EXPECT_EQ((*rs)["status"].toInt(), 200);
  EXPECT_EQ(countOf(client, "sqlite3", tb), 2);

  // db_trans: structured elements insert r3 + update r2, atomically.
  Json tr = argsWith("sqlite3");
  tr.add("sqls",
         Json(R"([{"table":"t_e2e","method":"insert","params":{"id":"r3","name":"carol","score":30}},{"table":"t_e2e","method":"update","params":{"score":99},"id":"r2"}])"));
  rs = client.callTool("db_trans", tr);
  ASSERT_TRUE(rs.has_value()) << rs.error().message;
  EXPECT_EQ((*rs)["status"].toInt(), 200);
  EXPECT_EQ(countOf(client, "sqlite3", tb), 3);

  // db_trans: raw text element with values binding.
  Json tr2 = argsWith("sqlite3");
  tr2.add("sqls",
          Json(R"json([{"text":"insert into t_e2e (id, name, score) values (?, ?, ?)","values":["r4","dave",40]}])json"));
  rs = client.callTool("db_trans", tr2);
  ASSERT_TRUE(rs.has_value()) << rs.error().message;
  EXPECT_EQ((*rs)["status"].toInt(), 200);
  EXPECT_EQ(countOf(client, "sqlite3", tb), 4);

  // db_delete: single row by id.
  Json del = argsWith("sqlite3");
  del.add("table", Json::str(tb));
  del.add("where", Json(R"({"id":"r4"})"));
  rs = client.callTool("db_delete", del);
  ASSERT_TRUE(rs.has_value()) << rs.error().message;
  EXPECT_EQ((*rs)["status"].toInt(), 200);
  EXPECT_EQ(countOf(client, "sqlite3", tb), 3);

  // db_delete with a condition-only where is REJECTED with guidance.
  Json badDel = argsWith("sqlite3");
  badDel.add("table", Json::str(tb));
  badDel.add("where", Json(R"({"name":"carol"})"));
  rs = client.callTool("db_delete", badDel);
  ASSERT_FALSE(rs.has_value());
  EXPECT_NE(rs.error().message.find("db_query"), std::string::npos);

  // db_delete where:{} = delete all.
  Json delAll = argsWith("sqlite3");
  delAll.add("table", Json::str(tb));
  delAll.add("where", Json("{}"));
  rs = client.callTool("db_delete", delAll);
  ASSERT_TRUE(rs.has_value()) << rs.error().message;
  EXPECT_EQ((*rs)["status"].toInt(), 200);
  EXPECT_EQ(countOf(client, "sqlite3", tb), 0);

  // completion: table names starting with "t_e2".
  Json extra;
  extra.add("ref", Json(R"({"type":"ref/prompt","name":"table","arguments":{"db":"sqlite3"}})"));
  Json arg;
  arg.add("name", Json::str("table"));
  arg.add("value", Json::str("t_e2"));
  extra.add("argument", std::move(arg));
  auto comp = client.request("completion/complete", extra);
  ASSERT_TRUE(comp.has_value());
  EXPECT_NE((*comp).toString().find("t_e2e"), std::string::npos);
}

// ---- the open-box jsonfile backend (same semantics, no SQL) ----------------------

TEST(ZormMcpE2E, JsonfileParity) {
  Child child = spawnChild(false, false);  // jsonfile path needs no --allow-sql
  PipeTransport t(child);
  Client client("e2e", "1.0");
  client.connect(t);
  const char* tb = "t_e2e_json";
  ASSERT_NO_FATAL_FAILURE(clearJsonTable(client, tb));

  // db_insert auto-generates the id when absent.
  Json ins = argsWith("jsonfile");
  ins.add("table", Json::str(tb));
  ins.add("data", Json(R"({"name":"ada","score":11})"));
  auto rs = client.callTool("db_insert", ins);
  ASSERT_TRUE(rs.has_value()) << rs.error().message;
  expectEnvelope(*rs);
  EXPECT_EQ((*rs)["status"].toInt(), 200);
  EXPECT_EQ(countOf(client, "jsonfile", tb), 1);

  // db_describe_table reads the storage format's columns array - no rows
  // needed, no SQL involved.
  auto desc = client.callTool("db_describe_table",
                              Json(R"({"db":"jsonfile","table":"t_e2e_json"})"));
  ASSERT_TRUE(desc.has_value()) << desc.error().message;
  EXPECT_NE((*desc).toString().find("name"), std::string::npos);
  EXPECT_NE((*desc).toString().find("score"), std::string::npos);

  // Smart query on jsonfile (202 semantics = empty result, still 200 here).
  Json q = argsWith("jsonfile");
  q.add("table", Json::str(tb));
  q.add("lks", Json(R"({"name":"%ad%"})"));
  rs = client.callTool("db_query", q);
  ASSERT_TRUE(rs.has_value());
  EXPECT_NE((*rs).toString().find("ada"), std::string::npos);

  // delete-all works even though jsonfile's SQL shim cannot express it.
  ASSERT_NO_FATAL_FAILURE(clearJsonTable(client, tb));
  EXPECT_EQ(countOf(client, "jsonfile", tb), 0);
}

// ---- MRTR write confirmation ------------------------------------------------------

TEST(ZormMcpE2E, MrtrWriteConfirmation) {
  Child child = spawnChild(true, false);  // confirmation ON, no --allow-sql
  PipeTransport t(child);
  Client client("e2e", "1.0");
  client.connect(t);
  const char* tb = "e2e_mrtr";
  ASSERT_NO_FATAL_FAILURE(clearJsonTable(client, tb));

  // First call: input_required + requestState + elicitation inputRequests.
  Json args = argsWith("jsonfile");
  args.add("table", Json::str(tb));
  args.add("data", Json(R"({"note":"e2e-zorm-mcp"})"));
  auto first = client.callTool("db_insert", args);
  ASSERT_TRUE(first.has_value());
  expectEnvelope(*first);
  EXPECT_EQ((*first)["resultType"].toString(), "input_required");
  const std::string state = (*first)["requestState"].toString();
  EXPECT_FALSE(state.empty());
  const Json confirmReq = member(*first, "inputRequests");
  ASSERT_TRUE(confirmReq.isObject());
  EXPECT_EQ(confirmReq["confirm"]["method"].toString(), "elicitation/create");
  // Confirmation text names the target (db.table) and how to approve.
  const std::string msg = (*first)["content"][0]["text"].toString();
  EXPECT_NE(msg.find("jsonfile." + std::string(tb)), std::string::npos);
  EXPECT_NE(msg.find(state), std::string::npos);

  // Approved retry: original args + requestState + inputResponses -> execute.
  Json retry = argsWith("jsonfile");
  retry.add("table", Json::str(tb));
  retry.add("data", Json(R"({"note":"e2e-zorm-mcp"})"));
  retry.add("requestState", Json::str(state));
  retry.add("inputResponses", Json(R"({"confirm":{"approved":true}})"));
  auto second = client.callTool("db_insert", retry);
  ASSERT_TRUE(second.has_value());
  expectEnvelope(*second);
  EXPECT_EQ((*second)["status"].toInt(), 200);
  ASSERT_EQ(countOf(client, "jsonfile", tb), 1);

  // Rejected retry: staging again then answering false must NOT write.
  Json stage2 = argsWith("jsonfile");
  stage2.add("table", Json::str(tb));
  stage2.add("data", Json(R"({"note":"e2e-rejected"})"));
  auto req2 = client.callTool("db_insert", stage2);
  ASSERT_TRUE(req2.has_value());
  EXPECT_EQ((*req2)["resultType"].toString(), "input_required");
  Json decline = argsWith("jsonfile");
  decline.add("table", Json::str(tb));
  decline.add("data", Json(R"({"note":"e2e-rejected"})"));
  decline.add("requestState", Json::str((*req2)["requestState"].toString()));
  decline.add("inputResponses", Json(R"({"confirm":{"approved":false}})"));
  auto declined = client.callTool("db_insert", decline);
  ASSERT_TRUE(declined.has_value());
  EXPECT_EQ((*declined)["resultType"].toString(), "complete");
  EXPECT_NE((*declined).toString().find("cancelled"), std::string::npos);
  ASSERT_EQ(countOf(client, "jsonfile", tb), 1);

  // Unknown requestState -> InvalidParams with re-stage guidance.
  Json stale = argsWith("jsonfile");
  stale.add("table", Json::str(tb));
  stale.add("data", Json(R"({"note":"x"})"));
  stale.add("requestState", Json::str("st-999999"));
  stale.add("inputResponses", Json(R"({"confirm":{"approved":true}})"));
  auto errRs = client.callTool("db_insert", stale);
  ASSERT_FALSE(errRs.has_value());
  EXPECT_EQ(errRs.error().code, McpErrorCode::InvalidParams);
  EXPECT_NE(errRs.error().message.find("stage a new one"), std::string::npos);

  // Flat-form approval ({"approved":true} at top level) also executes.
  Json stage3 = argsWith("jsonfile");
  stage3.add("table", Json::str(tb));
  stage3.add("data", Json(R"({"note":"e2e-flat"})"));
  auto req3 = client.callTool("db_insert", stage3);
  ASSERT_TRUE(req3.has_value());
  Json flat = argsWith("jsonfile");
  flat.add("table", Json::str(tb));
  flat.add("data", Json(R"({"note":"e2e-flat"})"));
  flat.add("requestState", Json::str((*req3)["requestState"].toString()));
  flat.add("inputResponses", Json(R"({"approved":true})"));
  auto flatRs = client.callTool("db_insert", flat);
  ASSERT_TRUE(flatRs.has_value());
  EXPECT_EQ((*flatRs)["status"].toInt(), 200);

  ASSERT_NO_FATAL_FAILURE(clearJsonTable(client, tb));
  EXPECT_EQ(countOf(client, "jsonfile", tb), 0);
}

// ---- MRTR on the trans tool + graceful shutdown ------------------------------------

TEST(ZormMcpE2E, MrtrTransAndShutdown) {
  Child child = spawnChild(true, true);  // confirmation ON, --allow-sql
  PipeTransport t(child);
  Client client("e2e", "1.0");
  client.connect(t);
  ASSERT_NO_FATAL_FAILURE(resetTable(client));

  // db_trans confirmation flow (impact text states the statement count).
  Json tr = argsWith("sqlite3");
  tr.add("sqls",
         Json(R"([{"table":"t_e2e","method":"insert","params":{"id":"r1","name":"ada","score":1}},{"table":"t_e2e","method":"insert","params":{"id":"r2","name":"bob","score":2}}])"));
  auto first = client.callTool("db_trans", tr);
  ASSERT_TRUE(first.has_value());
  expectEnvelope(*first);
  ASSERT_EQ((*first)["resultType"].toString(), "input_required");
  EXPECT_NE((*first)["content"][0]["text"].toString().find("2 statements"),
            std::string::npos);
  const std::string state = (*first)["requestState"].toString();
  EXPECT_EQ(countOf(client, "sqlite3", "t_e2e"), 0);  // nothing executed yet

  Json retry = argsWith("sqlite3");
  retry.add("sqls",
            Json(R"([{"table":"t_e2e","method":"insert","params":{"id":"r1","name":"ada","score":1}},{"table":"t_e2e","method":"insert","params":{"id":"r2","name":"bob","score":2}}])"));
  retry.add("requestState", Json::str(state));
  retry.add("inputResponses", Json(R"({"action":"accept"})"));
  auto second = client.callTool("db_trans", retry);
  ASSERT_TRUE(second.has_value()) << second.error().message;
  EXPECT_EQ((*second)["status"].toInt(), 200);
  EXPECT_EQ(countOf(client, "sqlite3", "t_e2e"), 2);

  // --db_exec_sql hidden without --allow-sql is covered elsewhere; here it is
  // registered and confirmed: reject once, approve once.
  Json sql = argsWith("sqlite3");
  sql.add("sql", Json::str("insert into t_e2e (id, name, score) values ('r3','carol',3)"));
  auto req = client.callTool("db_exec_sql", sql);
  ASSERT_TRUE(req.has_value());
  EXPECT_EQ((*req)["resultType"].toString(), "input_required");
  Json sqlRetry = argsWith("sqlite3");
  sqlRetry.add("sql", Json::str("insert into t_e2e (id, name, score) values ('r3','carol',3)"));
  sqlRetry.add("requestState", Json::str((*req)["requestState"].toString()));
  sqlRetry.add("inputResponses", Json(R"({"confirm":{"approved":true}})"));
  auto exec = client.callTool("db_exec_sql", sqlRetry);
  ASSERT_TRUE(exec.has_value()) << exec.error().message;
  EXPECT_EQ(countOf(client, "sqlite3", "t_e2e"), 3);
}

TEST(ZormMcpE2E, GracefulShutdownOnEof) {
  Child child = spawnChild(false, false);
  PipeTransport t(child);
  Client client("e2e", "1.0");
  client.connect(t);
  auto disc = client.discover();
  ASSERT_TRUE(disc.has_value());
  // Closing the pipe's write end signals EOF; the server should exit cleanly.
  child.closeStdin();
  int status = 0;
#ifdef _WIN32
  EXPECT_TRUE(WaitForSingleObject(child.pi.hProcess, 10000) == WAIT_OBJECT_0);
#else
  EXPECT_EQ(waitpid(child.pid, &status, 0), child.pid);
#endif
}
