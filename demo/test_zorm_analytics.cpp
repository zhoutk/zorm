// Zorm Analytics MCP - end-to-end test.
//
// Spawns the zorm_analytics_server executable as a child (real stdio pipes)
// and drives it with the mcp-cpp Client API. Covers the demo's whole surface:
// discover, tools/list, cross-DB smart queries (sqlite + jsonfile), db_count,
// resources (db://schema), completion, and the MRTR write-confirmation flow
// (input_required -> approved retry persists / rejected retry cancels).
//
// The child runs with --child (server mode) and a scratch data directory so
// the test is self-contained and idempotent.

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

Child spawnChild(bool confirmWrites) {
#ifdef ZORM_DLL_DIR
  // The server child loads zorm's runtime DLLs; make sure they resolve even
  // when ctest runs outside the run-test PATH setup.
  char pathBuf[8192] = {0};
  GetEnvironmentVariableA("PATH", pathBuf, sizeof(pathBuf));
  std::string newPath = ZORM_DLL_DIR ";" + std::string(pathBuf);
  SetEnvironmentVariableA("PATH", newPath.c_str());
#endif
  // Spawn the REAL analytics server (sibling of this test executable) - NOT
  // this test itself: the test's main is gtest_main with no --child handler,
  // so self-spawning would recursively run the whole suite (fork bomb).
  char testPath[MAX_PATH];
  GetModuleFileNameA(nullptr, testPath, MAX_PATH);
  std::string serverPath(testPath);
  const size_t slash = serverPath.find_last_of("\\/");
  serverPath = serverPath.substr(0, slash + 1) + "zorm_analytics_server.exe";

  std::string cmdline = std::string("\"") + serverPath + "\" --child";
  if (!confirmWrites) cmdline += " --yes";
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
Child spawnChild(bool confirmWrites) {
  // Spawn the real analytics server executable sitting next to this test
  // (never self-spawn: this test's main is gtest_main without --child).
  std::string self = "/proc/self/exe";
  char buf[4096] = {0};
  ssize_t n = readlink(self.c_str(), buf, sizeof(buf) - 1);
  std::string serverPath = (n > 0) ? std::string(buf, static_cast<size_t>(n)) : self;
  const size_t slash = serverPath.find_last_of('/');
  serverPath = serverPath.substr(0, slash + 1) + "zorm_analytics_server";

  int inPipe[2], outPipe[2];
  pipe(inPipe); pipe(outPipe);
  Child c;
  c.pid = fork();
  if (c.pid == 0) {
    dup2(inPipe[0], STDIN_FILENO);
    dup2(outPipe[1], STDOUT_FILENO);
    close(inPipe[0]); close(inPipe[1]); close(outPipe[0]); close(outPipe[1]);
    std::vector<char*> childArgv = {const_cast<char*>("zorm_analytics_server"),
                                    const_cast<char*>("--child"), nullptr};
    std::string yesFlag;
    if (!confirmWrites) {
      yesFlag = "--yes";
      childArgv.push_back(const_cast<char*>(yesFlag.c_str()));
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

}  // namespace

TEST(ZormAnalyticsE2E, DiscoverAndToolsList) {
  Child child = spawnChild(false);
  PipeTransport t(child);
  Client client("e2e", "1.0");
  client.connect(t);

  auto disc = client.discover();
  ASSERT_TRUE(disc.has_value());
  EXPECT_EQ((*disc)["resultType"].toString(), "complete");
  ASSERT_TRUE((*disc)["supportedVersions"].isArray());

  auto tools = client.listTools();
  ASSERT_TRUE(tools.has_value());
  const Json toolsArr = member(*tools, "tools");
  ASSERT_TRUE(toolsArr.isArray());
  // At minimum the 6 core tools (exec_sql hidden without --allow-sql).
  EXPECT_GE(toolsArr.size(), 6u);
}

TEST(ZormAnalyticsE2E, CrossDatabaseSmartQuery) {
  Child child = spawnChild(false);
  PipeTransport t(child);
  Client client("e2e", "1.0");
  client.connect(t);

  // sqlite: list tables.
  auto tables = client.callTool("db_list_tables", Json(R"({"db":"sqlite"})"));
  ASSERT_TRUE(tables.has_value());
  const std::string tablesText = (*tables).toString();
  EXPECT_NE(tablesText.find("orders"), std::string::npos);
  EXPECT_NE(tablesText.find("users"), std::string::npos);

  // sqlite: 2026 orders grouped by region with summed amounts.
  Json q;
  q.add("db", Json::str("sqlite"));
  q.add("table", Json::str("orders"));
  q.add("lks", Json(R"({"order_date":"2026"})"));
  q.add("group", Json::str("region"));
  q.add("sum", Json::str("amount"));
  auto qr = client.callTool("db_query", q);
  ASSERT_TRUE(qr.has_value());
  const Json data = member(*qr, "data");
  ASSERT_TRUE(data.isArray());
  EXPECT_GE(data.size(), 1u);  // at least one region
  EXPECT_NE((*qr).toString().find("central"), std::string::npos);

  // sqlite: products IN electronics.  (products 现在在 sqlite; jsonfile 是空输出库)
  Json p;
  p.add("db", Json::str("sqlite"));
  p.add("table", Json::str("products"));
  p.add("ins", Json(R"({"category":"electronics"})"));
  auto pr = client.callTool("db_query", p);
  ASSERT_TRUE(pr.has_value());
  EXPECT_NE((*pr).toString().find("gadget"), std::string::npos);

  // 比较操作符: price > 100 (zorm 的 >, 前缀, 之前被 schema 校验误拒, 回归保护)。
  Json pq;
  pq.add("db", Json::str("sqlite"));
  pq.add("table", Json::str("products"));
  pq.add("price", Json::str(">,100"));
  auto pr2 = client.callTool("db_query", pq);
  ASSERT_TRUE(pr2.has_value());
  EXPECT_NE((*pr2).toString().find("gadget"), std::string::npos);  // gadget 120 > 100

  // jsonfile 输出库: 空文件 -> 无表 (不再是硬编码的 4 张表)。
  auto jt = client.callTool("db_list_tables", Json(R"({"db":"jsonfile"})"));
  ASSERT_TRUE(jt.has_value());
  EXPECT_NE((*jt).toString().find("tables"), std::string::npos);

  // db_count.
  auto cnt = client.callTool("db_count", Json(R"({"db":"sqlite","table":"orders"})"));
  ASSERT_TRUE(cnt.has_value());
  EXPECT_NE((*cnt).toString().find("500"), std::string::npos);

  // Negative paths: unknown db and unknown table are InvalidParams (-32602).
  auto badDb = client.callTool("db_list_tables", Json(R"({"db":"oracle"})"));
  ASSERT_FALSE(badDb.has_value());
  EXPECT_EQ(badDb.error().code, McpErrorCode::InvalidParams);
  auto badTable = client.callTool("db_count", Json(R"({"db":"sqlite","table":"nope"})"));
  ASSERT_FALSE(badTable.has_value());
  EXPECT_EQ(badTable.error().code, McpErrorCode::InvalidParams);
  // Schema enforcement: size out of range -> rejected before the handler.
  auto badSize = client.callTool(
      "db_query", Json(R"({"db":"sqlite","table":"orders","size":100000})"));
  ASSERT_FALSE(badSize.has_value());
  EXPECT_EQ(badSize.error().code, McpErrorCode::InvalidParams);
}

TEST(ZormAnalyticsE2E, ResourcesAndCompletion) {
  Child child = spawnChild(false);
  PipeTransport t(child);
  Client client("e2e", "1.0");
  client.connect(t);

  auto schema = client.readResource("db://schema/sqlite/orders");
  ASSERT_TRUE(schema.has_value());
  EXPECT_NE((*schema).toString().find("region"), std::string::npos);
  EXPECT_NE((*schema).toString().find("amount"), std::string::npos);

  // completion: table names starting with "ord".
  Json extra;
  extra.add("ref", Json(R"({"type":"ref/prompt","name":"table"})"));
  Json arg;
  arg.add("name", Json::str("table"));
  arg.add("value", Json::str("ord"));
  extra.add("argument", std::move(arg));
  auto comp = client.request("completion/complete", extra);
  ASSERT_TRUE(comp.has_value());
  EXPECT_NE((*comp).toString().find("orders"), std::string::npos);
}

// MRTR write confirmation: first call returns input_required; approved retry
// persists; rejected retry cancels. The child runs with --yes for the read
// tests, but write confirmation must be exercised explicitly via the client
// echo of requestState + inputResponses.
TEST(ZormAnalyticsE2E, MrtrWriteConfirmation) {
  // Write confirmation ON: this is the very flow under test.
  Child child = spawnChild(true);
  PipeTransport t(child);
  Client client("e2e", "1.0");
  client.connect(t);

  // First call: expect input_required + requestState.
  Json args;
  args.add("db", Json::str("jsonfile"));
  args.add("table", Json::str("analytics"));
  Json data;
  data.add("note", Json::str("e2e-mrtr"));
  args.add("data", std::move(data));
  auto first = client.callTool("db_insert", args);
  ASSERT_TRUE(first.has_value());
  EXPECT_EQ((*first)["resultType"].toString(), "input_required");
  // Claude Code validates every tools/call result as CallToolResult: content
  // is mandatory even on input_required results. Guard against regression.
  EXPECT_TRUE((*first)["content"].isArray());
  const std::string state = (*first)["requestState"].toString();
  EXPECT_FALSE(state.empty());

  // Approved retry: add requestState + inputResponses.confirm.approved.
  Json retryArgs;
  retryArgs.add("db", Json::str("jsonfile"));
  retryArgs.add("table", Json::str("analytics"));
  Json data2;
  data2.add("note", Json::str("e2e-mrtr"));
  retryArgs.add("data", std::move(data2));
  retryArgs.add("requestState", Json::str(state));
  Json responses;
  Json confirm;
  confirm.add("approved", Json(true));
  responses.add("confirm", std::move(confirm));
  retryArgs.add("inputResponses", std::move(responses));
  auto second = client.callTool("db_insert", retryArgs);
  ASSERT_TRUE(second.has_value());
  EXPECT_EQ((*second)["resultType"].toString(), "complete");
  // Persisted - verify via count.
  auto cnt = client.callTool("db_count", Json(R"({"db":"jsonfile","table":"analytics"})"));
  ASSERT_TRUE(cnt.has_value());
  EXPECT_NE((*cnt).toString().find("\"total\""), std::string::npos);

  // Cleanup: remove the rows this test wrote so demo runs always start
  // from a clean analytics table (the demo's [7]-[9] steps expect that).
  auto rows = client.callTool("db_query",
                              Json(R"({"db":"jsonfile","table":"analytics","lks":{"note":"e2e-mrtr"}})"));
  if (rows.has_value()) {
    const Json data = member(*rows, "data");
    for (const auto& row : data.toVector()) {
      Json delArgs;
      delArgs.add("db", Json::str("jsonfile"));
      delArgs.add("table", Json::str("analytics"));
      Json where;
      where.add("id", Json::str(row["id"].toString()));
      delArgs.add("where", std::move(where));
      auto delFirst = client.callTool("db_delete", delArgs);
      if (delFirst.has_value() && (*delFirst)["resultType"].toString() == "input_required") {
        const std::string state = (*delFirst)["requestState"].toString();
        Json delRetry;
        delRetry.add("db", Json::str("jsonfile"));
        delRetry.add("table", Json::str("analytics"));
        Json where2;
        where2.add("id", Json::str(row["id"].toString()));
        delRetry.add("where", std::move(where2));
        delRetry.add("requestState", Json::str(state));
        Json delResponses;
        Json delConfirm;
        delConfirm.add("approved", Json(true));
        delResponses.add("confirm", std::move(delConfirm));
        delRetry.add("inputResponses", std::move(delResponses));
        (void)client.callTool("db_delete", delRetry);
      }
    }
  }
}

TEST(ZormAnalyticsE2E, GracefulShutdownOnEof) {
  Child child = spawnChild(false);
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
