# zorm-mcp / cpp-sdk 独立库实施计划

> 状态：**草稿 v1**（先想好再做；本文档随实施过程滚动更新，记录思路历程）
> 日期：2026-09-21 · 分支：`mcp`
> 关联：`docs/mcp/mcp-summary.md`（协议概要）、`docs/mcp/specification-2026-07-28/`（规范原文）、`docs/mcp/mcp-migration-plan.md`（整体迁移计划，本文是其中"协议层独立库"的落地深化）

---

## 0. 一句话目标

在 `thirds/` 下构建**独立可发布的 MCP C++ SDK**（工作名 `mcp-cpp`，命名空间 `MCP`）：
以官方 Tier-1（TS/Python/C#/Go/Rust）功能全覆盖 + 协议一致性（conformance）对齐为目标，
性能对标并超越 rust-sdk（零拷贝解析、arena 内存模型、无锁派发），并原生服务 zorm 的场景。

## 1. 已核实的输入（2026-09-21）

| 事项 | 结论 | 来源 |
| --- | --- | --- |
| 协议版本 | **2026-07-28** 为当前稳定规范；本文档所有协议描述以它为准 | spec changelog |
| 协议面 | `server/discover`、`initialize`(遗留兼容)、`tools/list|call`、`resources/*`、`prompts/*`、`completion/complete`、`elicitation/create`(客户端)、`sampling/create`(已弃用)、`roots/list`(已弃用)、`notifications/*`(cancelled/progress/message/list_changed)、`subscriptions/listen`(2026 新)、结果必须带 `resultType` | schema.ts 枚举（已从本地 schema.md 提取全部方法名） |
| 2026 版重大变化 | **无状态化**：`initialize` 握手移除、HTTP 端 `Mcp-Session-Id` 移除；客户端 per-request `_meta` 携带 `protocolVersion/clientCapabilities/clientInfo`；`subscribions/resource subscribe` → `subscriptions/listen`；`ping`/`logging/setLevel` 移除（logLevel 走请求 `_meta`）；错误码分区 `-32020~-32099` 归规范、`-32000~-32019` 遗留；`CacheableResult`（`ttlMs/cacheScope`）；MRTR 取代服务端发起请求；Tasks 转为扩展 `io.modelcontextprotocol/tasks` | changelog 9 条 major |
| zjson 数字模型 | **Int64/Uint64/Double 三态 NumberData**，`from_chars` 快路径，arena 零拷贝（StringView 借用解析文档内存），流式写出 `toString/dumpTo`；整数解析体积收回满足**协议 int64 保真**要求且无精度丢失；`Json(text)`/`add(key,text)` 有 `{`/`[` 开头嗅探约定（文本值需 `Json::str()` 包裹，历史 bug 类） | `thirds/zjson/zjson.hpp` L73~121、项目 CLAUDE.md |
| httplib | yhirose/cpp-httplib **v0.52.0**（MIT），单头 `thirds/httplib/httplib.h` + 现成 INTERFACE 目标（线程池定义/keepalive 调参已配好） | `thirds/httplib/CMakeLists.txt` |
| 工具链 | 主力：clang 22.1.8（MSYS2 clang64，libc++）；对照：MSVC（VS2026）、GCC 15（libstdc++）；CMake≥3.18 | 本机实测 |
| conformance 测试 | `modelcontextprotocol/conformance` 仓库（2026-01 开放）；Tier-1 = 对"适用必测项"100% 通过（可标注 pending/skip） | sdk-tiers.md |

### 1.1 C++ 标准成熟度实测（本机 clang 22 / libc++，逐头文件编译验证）

| 特性 | clang22(libc++) 本机 | GCC 15(libstdc++) | MSVC(VS2026) | 备注 |
| --- | --- | --- | --- | --- |
| `std::expected` | ✅ | ✅ 12.1+ | ✅ | 错误处理主力 |
| `std::format/print` | ✅ | ✅ 13/14+ | ✅ | 日志、schema 构造 |
| `std::flat_map` | ✅ | ✅ 15.1+ | ✅ | |
| `std::span` | ✅ | ✅ | ✅ | 传输层缓冲区 |
| `<generator>` | ❌ 头缺失 | ✅ 14.1+* | ✅ | *libstdc++ 为 TS/实验性 |
| `<inplace_vector>` | ❌ | ❌ | ✅ (`/std:c++latest`) | |
| `<contracts>` / `<meta>` | ❌ | ❌ | ❌ | 三家均缺，C++26 不实际 |
| `std::chrono tz` | ✅ | ✅ 14.1+ | ✅ | |

结论：**C++23 是"MSVC+GCC 均成熟、本机可验证"的最高甜点档**（clang22 的 libc++ 缺 generator/inplace_vector 可用 `std::function`/自写 trivially-copyable vector 规避，不碰虚表复杂度）。C++26 语言层（contracts/reflection）三家都未落地，纳入只会造成平台随机性。**推荐 C++23**，同时用 feature-test 宏防回退（见 §6.1）。
> **2026-09-21 Update（v1.1，结论已修订）**：用户提出“对 zorm（C++17 工程）而言，升 17→23 的进步是否值得放弃向下兼容”。逐子系统复核后把选型改判为 **C++17**，完整依据与替代方案见 §9 Update 2。本表保留为“特性可用性实测”原始记录，不再承担选型结论。
## 2. 产品定位与边界

### 2.1 定位（把范围写死，防止膨胀）

1. **协议核心库**（无任何 zorm 依赖）：JSON-RPC 2.0 编解码、五版规范（2024-11-05 → 2026-07-28）消息模型与版本协商、stdio + Streamable HTTP 传输、server/client 双侧能力调度、MRTR、取消、进度、订阅。
2. **类型系统与 Schema**：完整的 TS/Python 对等类型层（Request/Result/Error 三态 + `resultType` 多态判别）、JSON Schema 2020-12 校验器（tools inputSchema/outputSchema/elicitation schema 共用）。
3. **运行时**：服务器运行时（工具调度、能力协商、错误码、并发模型）、可选“server-builder” 便携 API（对标 Rust `Server::builder().add_tool(...)`、TS `McpServer.registerTool`）。
4. **C++17 为基、条件性现代化**：错误传播用内置 `McpResult`（检测到 `__cpp_lib_expected` 时直接别名 `std::expected`）——见 §9 Update 2 逐项对照表。

### 2.2 超出范围的（第一版不做，写入 ROADMAP）

- OAuth 2.1 / Client ID Metadata Documents（HTTP 鉴权）—— 依赖 WebSocket/TLS 生态选型，后补；
- Tasks 扩展（`io.modelcontextprotocol/tasks`）—— 2026 版降级为官方扩展，需异步调度器，放 v2；
- MCP Apps（UI 扩展）—— 超库职责；
- HTTP+SSD 旧传输（2025-03 已弃用）—— 仅保留兼容性探测（发 `GET` 409/405 判别旧服务器）,不完整实现。

## 3. 目录与 CMake 结构

```
thirds/mcp-cpp/
  include/mcp/                 # public, 3~5 个头
    mcp.hpp                    # 全功能单头入口（聚合若干分头）
    types.hpp                  # Json 类型 + Message 类型（Version/Capabilities/Tool/...）
    error.hpp                  # Error codes + expected<T,McpError>
    server.hpp                 # Server / builder API
    client.hpp                 # Client (stdio + http)
    transport.hpp              # ITransport / StdioTransport / HttpTransport
    schema.hpp                 # JSON Schema 2020-12 校验
    version.hpp                # MCP protocol version enum/strings
  src/
    types.cpp  error.cpp  schema.cpp  schema_check.cpp
    server.cpp  client.cpp
    transport_stdio.cpp  transport_http.cpp
    executor.cpp               # 单线程胃/多线程、task queue、 cancellation
  tests/
    test_jsonrpc.cpp           # RFC 全覆盖(无效 id/批量/严格解析)
    test_types_roundtrip.cpp   # schema.ts 全类型 ↔ Json 互转
    test_version.cpp           # 版本对齐矩阵测试(6 版规范)
    test_schema.cpp            # JSON Schema 2020-12 校验测试集
    test_stdio_e2e.cpp         # stdio 传输端到端（进程）
    test_http_e2e.cpp          # httplib 端到端
    test_mrtr.cpp test_cancel.cpp test_progress.cpp
  bench/
    bench_parse.cpp bench_dispatch.cpp    # vs "Rust 基线" 性能对比（见 §8）
  examples/
    zorm_server.cpp  minimal_server.cpp  client_probe.cpp
  CMakeLists.txt  README.md  LICENSE
```

- CMake 目标：`mcp`（静态库，**C++17**，与宿主工程一致，WIN32/ZORM 解耦）、`mcp::mcp` 做 alias、`mcp_tests`、`mcp_bench`。
- 集成到根工程：根 CMake `add_subdirectory(thirds/mcp-cpp)`，仅追加 `mcp` / `mcp_tests` 两个 target，👍 同时独立可 build（目录内自给自足，拷走即用）。
- 与 zjson 的关系：**协议核心库自带薄 JSON 抽象**（默认实现直接用 zjson 单头包 `thirds/zjson`，对外仅暴露 `mcp::Json` 前向声明+概念约束，使用方亦可注入 rapidjson 等适配器）。原因：独立发布不可能强绑 zorm 仓库路径。

## 4. 模块设计

### 4.1 类型层（对标 TS `types.ts`，~1500 行的 C++ 对应物）

```cpp
namespace mcp {
  // 协议版本（对应 TS LATEST_PROTOCOL_VERSION 与 6 个版本常量）
  enum class ProtocolVersion : uint8_t {
    V2024_11_05, V2025_03_26, V2025_06_18,
    V2025_11_25, V2026_07_28, Latest = V2026_07_28
  };
  std::string_view toString(ProtocolVersion);
  std::optional<ProtocolVersion> fromString(std::string_view);

  // 能力结构（2016 版用 variant，2026 版查 _meta）
  struct ClientCapabilities { /* fs/roots/experimental + extensions dict */ };
  struct ServerCapabilities { /* tools/resources/prompts/... + extensions */ };

  // 三态结果模型（2026 新）：complete / 输入需要 / 传输级错误
  using RpcResult = std::variant<CompleteResult, InputRequiredResult>;
  // Error：强类型错误码 + JSON-RPC code
  enum class McpErrorCode : int32_t { ... };  // ParseError=-32700, InvalidRequest=-32600,
                                              // MethodNotFound=-32601, InvalidParams=-32602, InternalError=-32603,
                                              // -32020 HeaderMismatch, -32021 MissingRequiredClientCapability,
                                              // -32022 UnsupportedProtocolVersion ...
  struct McpError { int32_t code; std::string message; Json data; };
  template<class T> using McpResult = std::expected<T, McpError>;
}
```

- 决策：**不用继承多态**表达 Result（Rust 用 enum 的 C++ 对应物是 `std::variant` + visitor；C# 的 `record` 对应物是 `struct` 聚合），消息层全部 value 语义 + `[[nodiscard]]`。
- schema 校验：**JSON Schema 2020-12** 校验器（支持 `$ref` 局部、`const/enum/type/minimum/...` 18 个强语义关键字 + 递归深度上限 + composition 分支上限——照 SEP-2106 的"resource bounds"）。第一版为**协议自保**（校验自家生成 schema 的一致性），完整 `$ref` 远程解析、自定义词表放 v2。

### 4.2 传输层

```cpp
class ITransport {
public:
  virtual ~ITransport() = default;
  // 读写闭环：读到一条 JSON-RPC 消息，写出一条或流式
  virtual McpResult<Message> readMessage() = 0;
  virtual McpResult<void>   writeMessage(const Message&) = 0;
};
```

- **stdio**：`_setmode`/unistd 双平台二进制模式；行分帧（`\n`）；**严格 UTF-8**（协议要求，校验非法序列 → `ParseError`）；`notifications/cancelled` 由调度器解释。
- **Streamable HTTP**：基于 `thirds/httplib`（v0.52）。单端点 POST；响应可为单 JSON 或 `text/event-stream`；按 changelog 第 4/9 条实现 `subscriptions/listen`（长连接 POST 流 + `io.modelcontextprotocol/subscriptionId`）；**无会话头**（2026 移除了 `Mcp-Session-Id`，防误设计）。httplib 自带 SSE 支持（v0.52 `set_chunked_content_provider`），线程池 64 并发由 thirds/httplib/CMakeLists.txt 现成参数控制。
- 传输与调度解耦：`ITransport` 完全非虚（模板参数或 std::function 回调注入），测试可用内存 mock 传输。

### 4.3 调度器/运行时（性能敏感区）

- **无锁派发设计**：单线程 event loop（`epoll/kqueue/IOCP` 抽象 → 先用 `std::this_thread` 阻塞读 + task ring 做零依赖版），入站消息 → (校验schema → dispatch → handler) 全程 arena JSON，**零 `std::string` 拷贝**（zjson 的 StringView 借用文档内存，生命周期由 arena 池管）。对齐 rust-sdk 的 `Arc<Mutex<>>` 路径，但免锁。
- 可选 TL-threads 模式：多 worker（每 worker 独立 arena），入站按 hash 分流，handler 声明线程亲和性。
- Handler 签名（概念约束，编译期拒绝非值语义 handler）：

```cpp
using ToolHandler = std::function<McpResult<CallToolResult>(const CallToolRequest&)>;
// 高性能路径：注册时提供 `context`（arena*out）给用户预留 buffer，避免二次分配
```

### 4.4 Server/Client 便携 API（对标五个 Tier-1 SDK 最佳实践）

```cpp
mcp::server::Builder b(ProtocolVersion::Latest);
b.with_name("zorm").with_version("0.1.0")
 .options({.pageSize=50, .requireConfirmationOnWrite=true})
 .add_tool(Tool{ .name="db_select", .description="...", .inputSchema=schema })
     .handle([](Ctx& c, const Json& args) -> McpResult<Json> {
        auto db = c.get<zorm::Idb>();  // zorm 在例子里，库本身不依赖
        return ...;
     });
auto server = b.build();
server.run(StdioTransport{});     // 或 server.serve_http("0.0.0.0", 8080)
```

- Client API 对标 python-sdk `ClientSession`：`connect(transport)` / `list_tools()` / `call_tool()` / `read_resource()` / `subscribe(...)`，全部 `McpResult<T>`。
- C++17 惯用法补强：builder 链式返回用引用限定 `T&& set_x(...)&&`（C++17 即可精确控制 `&&` 生命周期）；编译期 schema 注册用 `constexpr` 聚合 + `consteval` 前身（若检测到 C++20 编译器则启用 `consteval`，否则 `constexpr`）。

## 5. 与官方 Tier-1 SDK 的功能对齐矩阵（设计验收）

| 功能 | TS/src | Python | C# | Go | Rust | **mcp-cpp 覆盖阶段** |
| --- | --- | --- | --- | --- | --- | --- |
| JSON-RPC 2.0 严格编解码 | ✅ | ✅ | ✅ | ✅ | ✅ | P0 |
| 6 版协议版本协商（schema 见 V 版常量） | ✅ | ✅ | ✅ | ✅ | ✅ | P0 |
| stdio 传输 | ✅ | ✅ | ✅ | ✅ | ✅ | P0 |
| Streamable HTTP + SSE | ✅ | ✅ | ✅ | ✅ | ✅ | P0 |
| tools/resources/prompts 完整类型 | ✅ | ✅ | ✅ | ✅ | ✅ | P0 |
| 错误码 + `resultType` 三态 | ✅ | ✅ | ✅ | ✅ | ✅ | P0 |
| MRTR（input_required → inputResponses 重试） | ✅ | ✅ | ✅ | ✅ | ✅ | P0 |
| 取消 notifications/cancelled | ✅ | ✅ | ✅ | ✅ | ✅ | P0 |
| 进度 notifications/progress | ✅ | ✅ | ✅ | ✅ | ✅ | P1 |
| `server/discover`（2026 新 RPC） | ✅ | ✅ | ✅ | ✅ | ✅ | P0 |
| `subscriptions/listen`（2026 新） | ✅ | ✅ | ✅ | ✅ | ✅ | P0 |
| CacheableResult(ttlMs/cacheScope) | ✅ | ✅ | ✅ | ✅ | ✅ | P1 |
| completion/complete | ✅ | ✅ | ✅ | ✅ | ✅ | P1 |
| 客户端能力：elicitation(MRTR) | ✅ | ✅ | ✅ | ✅ | ✅ | P1 |
| Roots/Sampling（已弃用） | ✅ | ✅ | ✅ | ✅ | ✅ | **不实现**（SEP-2577；保留弃用握手兼容层） |
| Tasks 扩展 | E | E | E | E | E | v2 |
| OAuth/auth | ✅ | ✅ | ✅ | ✅ | ✅ | v2 |
| JSON Schema 校验 | TS 用 ajv | pydantic | data-annotation | gojsonschema | jsonschema | **自研轻量校验器** |
| **C++ 特色：arena 零拷贝 / 无锁派发** | — | — | — | — | — | **P0（差异化）** |

> 对被弃用的 Roots/Sampling：库**不实现**但保证消息不崩（透传 unknown method → MethodNotFound/忽略弃用通知）， HLS 兼容禁用层单测。

## 6. 工程约束（C++ 最佳实践 / 可发布性）

### 6.1 标准与编译器矩阵（v1.1 按 §9 Update 2 修订）

- **语言标准 = C++17**（与 zorm 主工程一致；`std=c++17` / `/std:c++17`）。
- 条件性 C++20/23 特性（可选加速，不构成门槛）：`__cpp_lib_expected >= 202202L` 则 `McpResult` 直接别名 `std::expected`，否则用内置 `Result` fallback；`__cpp_lib_three_way_comparison` 则启用 `<=>`；未被 C++17 允许的一切写成单一实现（不做双实现洪流）。
- 编译器矩阵 `(clang22, gcc≥11, msvc v19.3x) × (RelWithDebInfo, Debug)`；GCC 下限降为 11（Ubuntu 22.04 默认版本，最大化受众）。
- **依赖透明**：`thirds/zjson`（JSON）、`thirds/httplib`（HTTP，可选宏 `MCP_DISABLE_HTTP`）、无其他第三库；gtest 用根工程已有 `googletest-1.18.0`（tests 仅内部用，不随库发布）。

### 6.2 可('&')=独立发布检查单

- [ ] `thirds/mcp-cpp` 可 `git subtree split` 独立成仓（自给 CMake、无 zorm 路径/头依赖）
- [ ] 构建零警告 + `clang-tidy`（`modernize-*`、`bugprone-*`、`readability-identifier-naming`=MCP 前）
- [ ] `MCP_DISABLE_HTTP` 独立于 HTTP 构建（纯 stdio 闲置的低配设备）
- [ ] `include/` 防泄漏：CI grep 校验 public 头不出现 `zorm/`、`db_`、mysql/pg 符号
- [ ] ABI：PIMPL 隔离 `std::` 类型跨边界（`server.hpp` 用 `pimpl`，导出前向声明唯一）
- [ ] 版本文件 `version.txt` + semver + `MCP_SDK_VERSION` 宏；License：MIT + NOTICE 列 zjson/httplib
- [ ] README 含：功能矩阵、对标 TS/Go/Rust 的差异章节、zorm 集成示例

### 6.3 测试与（超越官方）质量项

1. **单元**：JSON-RPC 严析（无效 UTF-8、id 冲突、大整数 ±2^63、浮点精度、Deep nesting 限深）、schema 校验、版本兼容矩阵。
2. **协议一致性自测**：以 `docs/mcp/specification-2026-07-28/schema.md` 为 source-of-truth 写**生成器**：脚本扫 schema.ts 所在的 md，emit C++ 失败用例表 → 保证类型封包不可能漂移。（官方 conformance repo 开放后对齐）
3. **传输端到端**：stdio 起子进程真读写；HTTP 用 httplib 起 server/client 对跑 6 版协议。
4. **压力/并发**：1000 并发 tool calls、cancellation 风暴、流背压、arena 峰值内存回归。
5. **Mutation testing**（zorm 项目惯例）：关键分支（版本协商、错误码映射、MRTR 状态机）手工变异验证测试有效性。
6. **性能门禁**：bench 输出进 CI，防性能回退。

## 7. 里程碑（3 周粒度）

| 里程碑 | 内容 | 验收 |
| --- | --- | --- |
| **M0 骨架**（2d） | 目录/CMake/include 骨架、zjson 接入、CI 灰 | 三编译器全绿 |
| **M1 协议核心**（1w） | types/error/version、JSON-RPC 读写、stdio 传输 | test_jsonrpc 全绿，Inspector 可跑通 |
| **M2 server builder**（1w） | server 调度、tools/resources/prompts、MRTR、取消 | examples/minimal_server 过 Inspector；test_stdio_e2e |
| **M3 client + HTTP**（1w） | client API、Streamable HTTP、subscriptions/listen | test_http_e2e；setup `zorm_server.cpp` 连真库 |
| **M4 schema + 完备化**（1w） | JSON Schema 校验器、completion、Cacheable、版本矩阵 | 全部功能对齐 §5 |
| **M5 性能与发布**（1w） | bench 三项（解析/派发/端到端）vs rust 基线、文档、独立发布检查单 | 性能超 rust-sdk 同场景；三方检查单全勾 |

## 8. 性能目标（"超越 rust-sdk"的可验证化）

定义三个可复现基准（bench/）：

| 基准 | 内容 | rust-sdk 基线口径 | mcp-cpp 目标 |
| --- | --- | --- | --- |
| B1 parse | 1KB / 64KB JSON-RPC 消息吞吐 | serde_json + sdk 类型反序列化 | ≥1.5×（arena + from_chars 路径） |
| B2 dispatch | tools/call 派发+handler stub 回包 QPS | tokio 单任务 | ≥2×（免锁/免 future 开销） |
| B3 e2e | stdio 子进程 10k 消息往返时延 | 官方 stdio 传输 | ≥1.2×（避免 UTF-8 二次校验） |

- rust 基线用 `cargo bench` 于同机跑（M5 落实）；不达标则 profile（`perf`/VTune）后优化或调低承诺（文档如实记录）。
- 手段清单优先级：arena 复用（zjson 已有）→ 无锁 ring 入队 → 消息预解析指纹（先 peek method → 快路径 schema 校验）→ iovec writev 聚包。

## 9. 决策记录（本轮确认后开工）

### 2026-09-21 Update 2（v1.1）— 选型改判：**语言标准 = C++17**

**用户质询**：17→23 在本项目里到底有多大的进步？是否值得为它放弃向下兼容（zorm 主工程是 C++17）？
**回答**：逐子系统把 C++20/23 的候选特性按“真实贡献”过了一遍，结论是**收益集中在语法糖，无一项心动的结构性进步**。逐项如下：

| C++20/23 特性 | 在 mcp-cpp 的用途 | 若留在 C++17 的替代 | 真实代价 |
| --- | --- | --- | --- |
| `std::expected<T,E>` | 错误传播(Return type 惯例) | `McpResult` 手写(一个仅此用途的 60~80 行 `Result/Ok/Err` 模板，值语义、`[[nodiscard]]`) | +0.5d 一次性 |
| `std::variant`+visitor | 消息模型 | **C++17 就有**，无变化 | 0 |
| `std::format/print` | 日志/schema 构造 | zjson 已有流式/append 能力 + snappy 手写格式化；协议文本只有 `%s/%d/%llx` 级需求 | ~0 |
| `deducing this` | builder 链式 `&&` 生命周期 | 拷贝限定的链式（Rust builder 惯例同款）或显式 `move` | ~0 |
| concepts (20) | handler 概念约束 | C++17 `if constexpr`+ trait，真不规范时 `static_assert` | 易读性略降 |
| `constexpr string/vector`(20) | 编译期 schema 注册 | `consteval` 用不上时以运行时注册表起步 | v2 再说 |
| `<flat_map>` | 小型 method→handler 表 | `std::unordered_map`（方法名量级 <40，差异 ns 级） | ~0 |
| `std::span`(20) | 缓冲区视图 | **C++17 已有**（`<span>` 是 C++20 但 zjson 内部早已用 `string_view`，等价场景） | 0 |
| ranges/views(20/23) | 数据流水线 | 本库数据流是“一条消息一跳”，无长链流水线 | ~0 |
| coroutines/generator | 流式响应 | M1~M3 无流式需求；将来 SSE 处理器直接在传输层回调，不需要协程 | 0 |
| `std::format` fmt 风格日志 | 结构化日志 | MISSION: 与 zjson 共用 stderr 简单 logger | ~0 |

要点核实：**改动最大的三类兴趣点（性能、内存、并发）在 C++17→23 之间厂商零增量** ——性能来自 zjson arena（17 就有）、无锁/单线程调度（C 语义层，与版本无关）、`from_chars`（C++17 已收进 `<charconv>`）。
反过来，升 23 的**确定性成本**是：

1. **受众收窄**：GCC≥13/Clang≥17/MSVC v19.3x 起 —— 老发行版(Ubuntu 20.04/22.04 默认 GCC 9/11、CentOS/麒麟系、旧 MSVC)、以及“想把库直接拖进自家 C++17 工程”的用户全部出局。对一个以“官方没有 C++ SDK、你来补位”为卖点的库，受众是最核心资产。
2. **与宿主工程断裂**：mcp-cpp 既要在 zorm(C++17) 里集成，又要独立发布，双标准将使“一处修复两处 CI”成为常态（ABI/头兼容矩阵翻倍）。
3. **C++26 部分(备选C)** 原本就只做实验，不构成决策因素。

**改判结论**：库本体 **C++17**（与 zorm 同标准，`-fsigned-char`/构建体系照旧）；同时善用编译器无关的 C++20*语言*仅作为**条件性特性**（`__has_include`/`__cpp_lib_expected` 检测，检测到则用标准库实现，否则用内置 fallback —— 一个 `#if` 两行，交付“好心 C++20 加速”零负担）。

**性能/功能承诺不变**（§8 三基准 §5 全覆盖矩阵），且新增一条正向承诺：`mcp-cpp` 是"任意 C++17 工程里 copy 即用"的库 —— 这本身就是对抗"boost 貌似只有我们维护"这一现实的一个产品级卖点。

### 2026-09-21 Update 1（v1 初稿）— 自主决策（采用推荐方案，供用户事后复审）

用户当时不在线，按“自主决策、事后可推翻”处理，采用三项推荐值开工所需的准备已就绪：

1. ~~C++ 标准 = C++23~~（**v1.1 修订为 C++17**，见 Update 2）；缓解措施：CI 矩阵附带 GCC13 兼容检查（多数发行版 GCC 13 起有 expected/format）；
2. **并发模型 = 单线程 event loop 起步，多 worker 作为编译期开关（MCP_WORKERS=N）在 M4 性能门禁不达 B2 目标时启用**。避免"一步到位双模式"带来的 +3 天并发测试量前置。
3. **JSON 层 = 默认 zjson + `IJsonAdapter` 注入点**。arena 零拷贝收益全拿；注入点保证独立发布后第三方可替换 rapidjson/nlohmann。zjson 经 `thirds/mcp-cpp/thirds/zjson` 副本（或 CMake cache 路径）随库发布。

若您推翻任一项，修改本节并同步 §1.1/§4.3/§6.1 的相关设计，成本估算不变。

> 按此前约定：M0 动工前，请对 §9 三项表态；后续每个里程碑完成时，本文件随 debug/实测记录滚动更新（每节末尾附加"更新日志"小节）。

### 2026-09-21 Update 3 — M0 完成（骨架落地记录）

**验收口径修订（用户指示）**：测试/验收只在本机 clang 22.1.8（MSYS2 clang64, windows-gnu）完成；GCC/MSVC 矩阵降级为"编码时保持兼容"（平台差异一律走 feature-test 宏，不写死编译器分支），后期再验。

**对计划的三处修正（以核实为准）**：

1. **协议版本是 5 个，不是"6 版"**（§3 测试清单笔误；§4.1 enum 的 5 个枚举值为准），与规范版本史一致。
2. **错误码逐一对过本地规范**（`docs/mcp/specification-2026-07-28/basic/index.md` "Error codes"）：JSON-RPC 标准五码 + MCP 保留区 `-32020..-32099`，具名仅三个：`HeaderMismatch=-32020`、`MissingRequiredClientCapability=-32021`（其 `data.requiredCapabilities` 指向 `ClientCapabilities`）、`UnsupportedProtocolVersion=-32022`。另注意 2026 版把 resource-not-found 从 `-32002` 改为 `-32602`（InvalidParams），**没有**专用的 resource-not-found 码。`McpError.data`（JSON 载荷）推迟到 M1 与消息模型一起加——需先定 zjson arena 语义下 `data` 的所有权写法。
3. **头文件按里程碑落地**，不做全空占位骨架（避免"先占名字后填肉"的无效 diff）。M0 实际交付：`mcp.hpp`（聚合）、`version.hpp/.cpp`、`error.hpp`、`json.hpp`。

**设计记录**：

- `McpError.code` 用强类型 `enum class McpErrorCode : int32_t`（计划原文是裸 int32_t——强类型可防调用点传任意整数；上线到 wire 时 `static_cast`）。
- **错误返回双模式统一**：`McpResult<T>` 在 `__cpp_lib_expected >= 202202L` 时别名 `std::expected`，否则内置 `Result<T>`（variant 实现，`Result<void>` 特化用 monostate；成员面与 expected 一致：`has_value/value/error`）。`Ok(v)/Ok()/Err(code,msg)/Err(McpError)` 工厂返回 `ErrCarrier`，隐式转换为函数返回类型的 `McpResult<T>`——调用点 `return Err(...)` 无需写模板参数，且在两种模式下逐字相同。
- **踩坑**：`detail::makeErr` 必须放在 `McpResult` 别名声明之后（模板别名先声明后使用）——clang 直接指出实例化点，一次修复。
- **zjson 接入**：`mcp::Json = ZJSON::Json` 别名先行；`IJsonAdapter` 注入点按原计划推迟到 M1（等适配面最小集明确）。CMake 的 `MCP_ZJSON_DIR` 探测顺序：`./thirds/zjson`（独立发布布局）→ `../zjson`（仓库内布局）→ 报错。
- **gtest 发现顺序**：父工程已注册 → 仓库 `thirds/googletest-1.18.0` → 系统 GTest 包 → 警告并跳过测试。库本体零测试依赖。
- 首次配置时 `../../zjson` 路径算错过一次（`thirds/mcp-cpp` 的仓库兄弟是 `../zjson`），CMake 探测的 EXISTS 检查当即拦下——探测设计起了作用。

**M0 验收（本机 clang 22.1.8 / ninja）**：

- 根工程集成构建全绿（zormlib + 全部既有测试目标不受影响）。
- `ctest -R mcp_test`：`mcp_test_version`、`mcp_test_error` 全过（版本往返/未知串/错误码 static_assert 钉死 wire 值/Result 三态与 void/非默认构造 T）。
- **独立构建路径实测通过**：`cmake -S thirds/mcp-cpp -B <tmp> -DMCP_ZJSON_DIR=...` 独立配置、构建、测试全绿——"拷走即用"承诺成立。

### 2026-09-21 Update 4 — M1 完成（协议核心：类型/JSON-RPC/stdio 传输）

**开工前从规范核实的增量事实**（均以 `docs/mcp/specification-2026-07-28/` 为准）：

- **消息四型**：Request（id 为 string|integer，**禁 null**）、Notification（**禁 id**，含 null）、Result Response（`resultType` 必带；缺失按 legacy 读作 `complete`）、Error Response（schema 为 `id?: RequestId`——**无 null id**，读不出 id 时省略）。JSON-RPC 2.0 的"id null 即通知"读法被显式拒绝：id 成员存在但为 null → InvalidRequest， mismatch 要响亮地失败。
- **无状态**：`initialize` 握手在 2026 版已移除，每请求 `params._meta` 携带 `io.modelcontextprotocol/protocolVersion`（必填）+ `clientCapabilities`（必填）+ `clientInfo`（可选）；缺失 → `-32602`。双 era 服务器靠"请求带现代 `_meta` 还是带 `initialize`"分流——这是 M2 服务端的关键设计输入。
- **`server/discover`**：`DiscoverResult{resultType:"complete", supportedVersions[], capabilities, instructions?, ttlMs, cacheScope}`；stdio 客户端用"discover 探针 + 识别现代错误（-32022 等）"判定服务器 era，**fallback 不得绑定单一错误码**（legacy 服务器对未知方法报 -32601/-32602 或不响应）。
- **stdio 传输**：行分帧、禁嵌入换行、stdout 只准出消息、stderr 随意、EOF 是首要优雅关停信号；wire 格式与传输通道无关（socket/pipe 通吃）→ `StreamTransport` 注入 iostream 对，stdio 只是特例。
- **能力对象保持 Json 背托**（结构化类型，跟随规范演进），只对运行时会分支的字段提供 `capabilitiesInclude`/`capabilitiesIncludeExtension` 探针。全量类型化结构体会冻结一个规范还在活跃扩展的字段集——刻意偏离计划 §4.1 的"全字段结构体"路线。

**实现要点**：

- `parseMessage` 永不失败：任何输入产出 `variant<Request, Notification, Result, ErrorResponse, InvalidMessage>`，`InvalidMessage` 携带足以构造错误响应的全部信息（code + 可恢复的 id）。params 必须是 object（MCP 所有方法 params 皆对象）；数组（JSON-RPC 批量）显式拒绝；小数 id/错误码拒绝；±2^63 id 保真。
- 编码侧全部走 `Json::str()` 包文本（zjson 喂探约定），测试含"形如 JSON 的字符串值不被嗅探"用例。
- `Result<T>` 补齐 `operator->`/`operator*`，与 `std::expected` 成员面完全对齐。

**踩坑记录（两次，都值得留档）**：

1. **zjson API 误用**：`Json::str()` 是静态工厂（造字符串值），读字符串值的成员方法是 `toString()`；`Json::parse` 是 private，公开入口是 `Json::ParseJson(input, err)`。照 zorm 主工程既有惯用法（`params["id"].toString()`）校正。
2. **测试里的悬垂 UB（最有价值的一课）**：`asRequest(parseMessage(text))` 把指针指进**临时 variant**，全表达式结束即悬垂，后续读出垃圾——5 个测试报"bad_variant_access"看似库 bug，实为测试 UB；而同一个库用命名局部变量探针全过。教训：**对返回 variant 的函数，先落局部变量再 `get_if`**。已把全部 20 处调用点改为两步模式。另：ctest 前先确认没跑陈旧 exe（项目 CLAUDE.md 已有此坑记录，这次 cwd 漂移又踩了一次变体）。

**M1 验收（本机 clang 22.1.8 / ninja）**：5 个 CTest 注册全绿（version/error/jsonrpc/types/transport，约 60 用例）。计划里"Inspector 可跑通"一项推迟到 M2（需要真 server 进程才有可接对象）。计划中的 `test_stdio_e2e`（子进程往返）挪到 M2 与 minimal_server 一起做——没有 handler 循环的 stdio e2e 只能测回声，意义有限。

---
*文档维护规则：每个里程碑完成后，把遇到的偏差、性能实测数字、被否决的设计记录到对应用节下新增「Update YYYY-MM-DD」条目，原文不删。*
