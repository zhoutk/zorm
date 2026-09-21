// B3 - stdio end-to-end latency: spawn this executable as a child MCP server,
// measure 10k tools/call round trips over real OS pipes.
//
// Usage: mcp_bench_e2e [roundtrips]

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#else
#include <unistd.h>
#include <sys/wait.h>
#endif

#include <mcp/mcp.hpp>

#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

using namespace mcp;
using namespace mcp::server;
using namespace mcp::client;

#ifdef _WIN32
namespace {

struct PipeChild {
  HANDLE inWrite = nullptr, outRead = nullptr;
  PROCESS_INFORMATION pi{};

  ~PipeChild() {
    if (inWrite) CloseHandle(inWrite);
    if (outRead) CloseHandle(outRead);
    if (pi.hProcess) {
      WaitForSingleObject(pi.hProcess, 5000);
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
};

PipeChild spawnSelf() {
  char exePath[MAX_PATH];
  GetModuleFileNameA(nullptr, exePath, MAX_PATH);
  std::string cmdline = std::string("\"") + exePath + "\" --bench-child";
  SECURITY_ATTRIBUTES sa{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
  HANDLE inRead = nullptr, outWrite = nullptr;
  PipeChild c;
  CreatePipe(&inRead, &c.inWrite, &sa, 0);
  CreatePipe(&c.outRead, &outWrite, &sa, 0);
  SetHandleInformation(c.inWrite, HANDLE_FLAG_INHERIT, 0);
  SetHandleInformation(c.outRead, HANDLE_FLAG_INHERIT, 0);
  STARTUPINFOA si{};
  si.cb = sizeof(si);
  si.hStdInput = inRead;
  si.hStdOutput = outWrite;
  si.dwFlags |= STARTF_USESTDHANDLES;
  CreateProcessA(nullptr, cmdline.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr, &si, &c.pi);
  CloseHandle(inRead);
  CloseHandle(outWrite);
  return c;
}

}  // namespace
#else
namespace {

struct PipeChild {
  int inWrite = -1, outRead = -1;
  pid_t pid = -1;

  ~PipeChild() {
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
};

PipeChild spawnSelf() {
  int inPipe[2], outPipe[2];
  pipe(inPipe);
  pipe(outPipe);
  PipeChild c;
  c.pid = fork();
  if (c.pid == 0) {
    dup2(inPipe[0], STDIN_FILENO);
    dup2(outPipe[1], STDOUT_FILENO);
    close(inPipe[0]); close(inPipe[1]); close(outPipe[0]); close(outPipe[1]);
    execl("/proc/self/exe", "mcp_bench_e2e", "--bench-child", static_cast<char*>(nullptr));
    _exit(127);
  }
  close(inPipe[0]);
  close(outPipe[1]);
  c.inWrite = inPipe[1];
  c.outRead = outPipe[0];
  return c;
}

}  // namespace
#endif

namespace {

int runChildServer() {
  Builder b;
  b.name("bench-child").version("1.0");
  b.simpleTool("echo", "echoes", [](Ctx& c) -> McpResult<Json> {
    Json result;
    result.add("echoed", Json::str(c.params["arguments"]["text"].toString()));
    return Ok(std::move(result));
  });
  auto server = b.build();
  StdioTransport transport;
  return server->run(transport).has_value() ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
  for (int i = 1; i < argc; ++i) {
    if (std::string(argv[i]) == "--bench-child") return runChildServer();
  }
  const int rounds = (argc > 1) ? std::atoi(argv[1]) : 10000;

  PipeChild child = spawnSelf();
  Json params = encodeRequestMeta("2026-07-28", Json(R"({})"), std::nullopt);
  params.add("name", Json::str("echo"));
  Json arguments;
  arguments.add("text", Json::str("x"));
  params.add("arguments", std::move(arguments));

  // The child assigns fresh ids per request only via the same id; the stateless
  // server does not care, but correlation does - use incrementing ids.
  std::string line;
  std::string response;
  uint64_t sink = 0;

  const auto start = std::chrono::steady_clock::now();
  for (int i = 0; i < rounds; ++i) {
    const std::string request = encodeRequest(RequestId(static_cast<int64_t>(i)), "tools/call", params);
    if (!child.writeLine(request)) {
      std::fputs("child stdin closed\n", stderr);
      return 1;
    }
    if (!child.readLine(response)) {
      std::fputs("child stdout closed\n", stderr);
      return 1;
    }
    sink += response.size();
  }
  const auto end = std::chrono::steady_clock::now();

  if (sink == 0) return 1;  // defeat optimization
  const double secs = std::chrono::duration<double>(end - start).count();
  std::printf("B3 stdio e2e\n");
  std::printf("  %d round trips in %.3fs: %10.0f msg/s  (%.2f us/roundtrip)\n",
              rounds, secs, rounds / secs, 1e6 * secs / rounds);
  return 0;
}
