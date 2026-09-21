// stdio end-to-end: spawns this executable in child mode (a real MCP server
// over the real stdio transport), talks newline-framed JSON-RPC over OS pipes,
// and verifies graceful shutdown via stdin EOF.

#include <mcp/mcp.hpp>

#include <gtest/gtest.h>

#include <cstdio>
#include <optional>
#include <string>
#include <vector>

using namespace mcp;
using namespace mcp::server;

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>

namespace {

struct Child {
  HANDLE inWrite = nullptr;
  HANDLE outRead = nullptr;
  PROCESS_INFORMATION pi{};

  ~Child() {
    if (inWrite) CloseHandle(inWrite);
    if (outRead) CloseHandle(outRead);
    if (pi.hProcess) {
      WaitForSingleObject(pi.hProcess, 10000);
      CloseHandle(pi.hProcess);
      CloseHandle(pi.hThread);
    }
  }

  bool writeLine(const std::string& line) {
    DWORD written = 0;
    const std::string buf = line + "\n";
    return WriteFile(inWrite, buf.data(), static_cast<DWORD>(buf.size()), &written, nullptr) &&
           written == buf.size();
  }

  void closeStdin() {
    if (inWrite) {
      CloseHandle(inWrite);
      inWrite = nullptr;  // EOF: the server's graceful shutdown signal
    }
  }

  std::optional<std::string> readLine() {
    std::string line;
    char ch = 0;
    DWORD got = 0;
    while (ReadFile(outRead, &ch, 1, &got, nullptr) && got == 1) {
      if (ch == '\n') return line;
      if (ch != '\r') line.push_back(ch);
    }
    if (!line.empty()) return line;
    return std::nullopt;
  }
};

Child spawnSelf() {
  char exePath[MAX_PATH];
  GetModuleFileNameA(nullptr, exePath, MAX_PATH);
  std::string cmdline = std::string("\"") + exePath + "\" --mcp-serve-child";

  SECURITY_ATTRIBUTES sa{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
  HANDLE inRead = nullptr, outWrite = nullptr;
  Child child;
  EXPECT_TRUE(CreatePipe(&inRead, &child.inWrite, &sa, 0));
  EXPECT_TRUE(CreatePipe(&child.outRead, &outWrite, &sa, 0));
  EXPECT_TRUE(SetHandleInformation(child.inWrite, HANDLE_FLAG_INHERIT, 0));
  EXPECT_TRUE(SetHandleInformation(child.outRead, HANDLE_FLAG_INHERIT, 0));

  STARTUPINFOA si{};
  si.cb = sizeof(si);
  si.hStdInput = inRead;
  si.hStdOutput = outWrite;
  si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
  si.dwFlags |= STARTF_USESTDHANDLES;
  EXPECT_TRUE(CreateProcessA(nullptr, cmdline.data(), nullptr, nullptr, TRUE, 0, nullptr,
                             nullptr, &si, &child.pi));
  EXPECT_TRUE(child.inWrite != nullptr && child.outRead != nullptr && child.pi.hProcess != nullptr);
  CloseHandle(inRead);
  CloseHandle(outWrite);
  return child;
}

}  // namespace

#else  // POSIX

#include <csignal>
#include <unistd.h>
#include <sys/wait.h>

namespace {

struct Child {
  int inWrite = -1;
  int outRead = -1;
  pid_t pid = -1;

  ~Child() {
    if (inWrite >= 0) close(inWrite);
    if (outRead >= 0) close(outRead);
    int status = 0;
    if (pid > 0) waitpid(pid, &status, 0);
  }

  bool writeLine(const std::string& line) {
    const std::string buf = line + "\n";
    return ::write(inWrite, buf.data(), buf.size()) == static_cast<ssize_t>(buf.size());
  }

  void closeStdin() {
    if (inWrite >= 0) {
      close(inWrite);
      inWrite = -1;
    }
  }

  std::optional<std::string> readLine() {
    std::string line;
    char ch = 0;
    ssize_t got = 0;
    while ((got = ::read(outRead, &ch, 1)) == 1) {
      if (ch == '\n') return line;
      if (ch != '\r') line.push_back(ch);
    }
    if (!line.empty()) return line;
    return std::nullopt;
  }
};

Child spawnSelf() {
  std::vector<char*> argv = {program_invocation_name, const_cast<char*>("--mcp-serve-child"),
                             nullptr};
  int inPipe[2], outPipe[2];
  EXPECT_EQ(pipe(inPipe), 0);
  EXPECT_EQ(pipe(outPipe), 0);
  Child child;
  child.pid = fork();
  EXPECT_GE(child.pid, 0);
  if (child.pid == 0) {
    dup2(inPipe[0], STDIN_FILENO);
    dup2(outPipe[1], STDOUT_FILENO);
    close(inPipe[0]); close(inPipe[1]); close(outPipe[0]); close(outPipe[1]);
    execv(argv[0], argv.data());
    _exit(127);
  }
  close(inPipe[0]);
  close(outPipe[1]);
  child.inWrite = inPipe[1];
  child.outRead = outPipe[0];
  return child;
}

}  // namespace

#endif

namespace {

// The child-mode server: same shape as examples/minimal_server.cpp.
int runChildServer() {
  Builder b;
  b.name("e2e-child").version("0.1.0");
  b.simpleTool("echo", "echoes", [](Ctx& c) -> McpResult<Json> {
    Json result;
    result.add("echoed", Json::str(c.params["arguments"]["text"].toString()));
    return Ok(std::move(result));
  });
  Server server = b.build();
  StdioTransport transport;
  return server.run(transport).has_value() ? 0 : 1;
}

std::string makeMeta() {
  Json params = encodeRequestMeta("2026-07-28", Json(R"({})"), std::nullopt);
  return encodeRequest(RequestId(std::string("e2e-1")), "tools/list", params);
}

}  // namespace

TEST(StdioE2E, RoundtripAndGracefulShutdown) {
  Child child = spawnSelf();

  // discover
  Json discoverParams = encodeRequestMeta("2026-07-28", Json(R"({})"), std::nullopt);
  ASSERT_TRUE(child.writeLine(
      encodeRequest(RequestId(std::string("d1")), "server/discover", discoverParams)));
  const auto discoverLine = child.readLine();
  ASSERT_TRUE(discoverLine.has_value());
  {
    ParsedMessage parsed = parseMessage(*discoverLine);
    const auto* result = std::get_if<JsonRpcResult>(&parsed);
    ASSERT_NE(result, nullptr);
    EXPECT_EQ(result->result["resultType"].toString(), "complete");
    ASSERT_TRUE(result->result["supportedVersions"].isArray());
    EXPECT_GE(result->result["supportedVersions"].size(), 1);
  }

  // tools/list + tools/call
  ASSERT_TRUE(child.writeLine(makeMeta()));
  const auto listLine = child.readLine();
  ASSERT_TRUE(listLine.has_value());
  {
    ParsedMessage parsed = parseMessage(*listLine);
    const auto* result = std::get_if<JsonRpcResult>(&parsed);
    ASSERT_NE(result, nullptr);
    EXPECT_EQ(result->result["tools"].size(), 1);
  }

  Json callParams = encodeRequestMeta("2026-07-28", Json(R"({})"), std::nullopt);
  callParams.add("name", Json::str("echo"));
  Json arguments;
  arguments.add("text", Json::str("over real pipes"));
  callParams.add("arguments", std::move(arguments));
  ASSERT_TRUE(child.writeLine(
      encodeRequest(RequestId(std::string("c1")), "tools/call", callParams)));
  const auto callLine = child.readLine();
  ASSERT_TRUE(callLine.has_value());
  {
    ParsedMessage parsed = parseMessage(*callLine);
    const auto* result = std::get_if<JsonRpcResult>(&parsed);
    ASSERT_NE(result, nullptr);
    EXPECT_EQ(result->result["echoed"].toString(), "over real pipes");
  }

  // stdin EOF -> server exits gracefully
  child.closeStdin();
  EXPECT_EQ(child.readLine(), std::nullopt);
}

int main(int argc, char** argv) {
  for (int i = 1; i < argc; ++i) {
    if (std::string(argv[i]) == "--mcp-serve-child") {
      return runChildServer();
    }
  }
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
