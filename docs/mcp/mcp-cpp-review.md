# zorm 与 mcp-cpp 的关系评审报告

> 评审对象：`thirds/mcp-cpp`（独立 MCP C++ SDK，M0–M5 已完成）
> 评审日期：2026-09-22 · 分支 `mcp`
> 评审方式：源码逐文件核对 + 实际构建 + 全量测试 + 基准复跑 + PLAN.md 承诺兑现比对
> 关联：`./mcp-summary.md`（协议概要）、`./mcp-migration-plan.md`（迁移计划，本文同步更新）

---

## 一、结论先行

**mcp-cpp 是一份完成度高、协议意识强、工程纪律好的独立库**，其功能面与官方 Tier-1 SDK 对齐（§5 矩阵除有意不做的弃用项全部落地），测试与基准真实可复跑。评审中发现 **1 个真实缺陷（HTTP SSE 订阅流挂起）已当场修复并提交**（见 §五）。

**zorm 与 mcp-cpp 的关系可以这样概括**：

> **mcp-cpp 是 zorm 的"AI 接入层"，zorm 是 mcp-cpp 的"头号集成示例"——两者互为对方的第一用户，但库本体保持零耦合。**

- mcp-cpp **不依赖 zorm**（独立可发布，`include/` 无任何 zorm/db 符号），zorm 只是它的一个 consumer；
- zorm 通过 `examples/zorm_server.cpp` 把数据库暴露为 MCP 工具，是 mcp-cpp 的**活文档与验收样板**；
- 两者共享 zjson 作为 JSON 层（zorm 是 zjson 的作者方，mcp-cpp 复用其 arena 能力），但通过 `IJsonAdapter` 注入点保持可替换。

---

## 二、mcp-cpp 实际交付盘点（与宣称核对）

### 2.1 代码规模与结构（实测）

| 项 | 实测 | 说明 |
| --- | --- | --- |
| 核心代码 | ~5000 行（include 9 头 + src 11 文件） | `mcp.hpp` 聚合入口；分层：error→version→jsonrpc→types→transport→server/client→schema→http |
| 测试 | 10 个 CTest 注册，104+ 单元断言 + 2 e2e | 覆盖 JSON-RPC 31 项、server 19 项、schema 12 项、版本 5 项等 |
| 基准 | 3 个（B1 parse / B2 dispatch / B3 e2e） | 可复跑，`--bench-child` 自 spawn 模式 |
| 示例 | minimal_server（Inspector 就绪）+ zorm_server | zorm_server 仅仓库内构建（TARGET zormlib 守卫） |
| 构建 | standalone + 子目录双模式 | `MCP_ZJSON_DIR` / `MCP_DISABLE_HTTP` 可配 |

### 2.2 协议合规性核对（重点，逐项）

对照 2026-07-28 规范与本地 schema.md，确认：

| 协议点 | 实现 | 核对结果 |
| --- | --- | --- |
| JSON-RPC 2.0 MCP 化（id 禁 null、无批量、params 对象化） | `jsonrpc.cpp` 严格解码，错误携带可恢复 id | ✅ 与规范逐条一致 |
| 结果 `resultType` 三态 + 兼容缺省 | `server.cpp` 自动补 `"complete"`；缺省按规范视为 complete | ✅ |
| 错误码分区（`-32000~-32019` 遗留 / `-32020~-32099` 规范） | `error.hpp` 三码 + 保留区边界 | ✅ 含 `-32022` 版本协商 |
| 无状态化：per-request `_meta`（protocolVersion/clientCapabilities 必填） | `types.hpp` RequestMeta 解码，缺省报 `-32602` | ✅ 2026 版核心差异 |
| `server/discover`（supportedVersions/capabilities/ttl/cacheScope/serverInfo） | `handleDiscover` 全字段 | ✅ |
| `subscriptions/listen` + ack + filter 回显 | server 注册 + HTTP SSE provider | ✅（修复后） |
| 分页（不透明游标 + nextCursor） | `handlePaginatedList`，非法游标拒绝 | ✅ |
| CacheableResult（ttlMs/cacheScope） | discover + list 均带 | ✅ |
| HTTP 头镜像（MCP-Protocol-Version/Mcp-Method/Mcp-Name + `Mcp-Param-*`） | `transport_http.cpp` validateHeaders + `x-mcp-header` 编解码 | ✅ 含 `-32020` HeaderMismatch |
| 404/405 区分、202 通知、Origin 防护（DNS rebinding） | 均已实现且有 e2e 覆盖 | ✅ |
| 弃用项 Roots/Sampling（SEP-2577） | 有意不实现；未知方法回 `-32601` | ✅ 符合规范建议 |

**结论：协议面与 2026-07-28 规范高度一致，没有发现协议级偏差。**

### 2.3 关键设计亮点（评审认可）

1. **`member()` 直取成员**：规避了 zjson `operator[]` 的深搜回退（会误读嵌套协议数据）——这是 zorm 项目 zjson 嗅探 bug 类教训的正确传导；
2. **`ITransport` 接口 + `LineCapture`**：server 逻辑可在内存传输上单测（无 IO 依赖），HTTP 层复用同一 dispatch；
3. **SSE 用独立连接 + Pump 队列**：订阅不阻塞请求读循环，`closeSubscriptions` 修复后生命周期干净；
4. **JSON Schema 校验带资源上限**（maxDepth/maxChecks）：照规范防 DoS-by-schema；
5. **C++17 基座 + 条件性 `std::expected`**：`McpResult` 在 `__cpp_lib_expected` 时别名标准库，否则内置 `Result`——符合 §9 Update 2 的 C++17 改判，且调用面一致；
6. **文档纪律**：PLAN.md 的 Update 3–8 如实记录每个里程碑的偏差（含 B1 未达标的诚实修订），README 与实现一致。

---

## 三、评审中发现的问题（按严重度）

### P0（已修复）— HTTP SSE 订阅流挂起

- **现象**：`HttpE2E.SubscriptionStreamDeliversNotifications` 测试进程无法退出（ctest 卡死）。
- **根因**：客户端 `subscribe()` 建立的长连接 SSE 流无关闭途径，后台 `Post` 线程永阻塞、析构无法 join；服务器端 `stop()` 未通知活跃 SSE pump，`svr.stop()` 等待 worker join 时 provider 仍阻塞在 15s `waitPop`。
- **修复**（commit `9b854b3`）：
  - `ITransport` 增虚方法 `closeSubscriptions()`（基类 no-op）；HTTP client 覆写：closed 标记 → `stop()` 活跃连接（SSE receiver 收到标记中止 + 1s 读超时）→ 等线程注销；
  - `Client::closeSubscriptions()` 透传；`HttpServerTransport::stop()` 先 close 所有 pump 再 `svr.stop()`；
  - 测试 RAII 关闭；连带修掉 `-Wunused-private-field`、`-Wreorder-ctor` 两警告。
- **验证**：ctest 10/10 全过（1.23s），订阅测试 1.02s 正常退出；独立 + 根工程双构建零警告。

### P2（建议，未阻塞）— 基准 B1 未达 §8 承诺

- PLAN.md Update 8 已诚实记录：B1 parse 1KB 0.79×、64KB 0.38×（serde_json 基线）；B2/B3 因 rust-sdk 需完整 tokio 栈无法单机对比。
- **评估**：这是**交付取舍而非缺陷**——`parseMessage` 的严格语义验证（M1 设计：正确性优先）比 serde 纯借用解析成本高；且 zjson 的 arena 借用视图未被利用（`parseJsonText` 对 string_view 拷贝一次）。
- **后续候选**（PLAN 已列）：零拷贝 `string_view` 解析、envelope 预扫描快路径。

### P3（观察）— 若干可改进点

1. `handleToolCall` 的 `inputSchema` 校验在每次 `tools/call` 全量跑（B2 已含该校验成本）——可做 schema 预编译缓存；
2. `cancellations_` 用 `std::map` + `shared_ptr<atomic_bool>`，单线程 dispatch 下可用 `unordered_map` + 栈标记优化；
3. HTTP SSE 的 keep-alive 15s 与 `closeSubscriptions` 的 1s 读超时是两套节拍，可统一为常量；
4. `zorm_server.cpp` 目前是单连接全局 `g_db`（注释已声明单线程 dispatch 前提）——若未来多 worker，需改为每请求租用连接（复用 zorm 的 `DbPool`）。

---

## 四、zorm ↔ mcp-cpp：关系与结合方式

### 4.1 关系定位

```
┌─────────────────────────────────────────────────────────┐
│  AI 客户端 (Claude Code / VS Code / Cursor / ...)         │
│    └ MCP Client                                          │
└───────────────┬─────────────────────────────────────────┘
                │ stdio（本地）/ Streamable HTTP（远程）
┌───────────────▼─────────────────────────────────────────┐
│  zorm_server（zorm 仓库内示例，~150 行）                   │
│    ├─ mcp-cpp（协议层：server runtime / tools / schema / │
│    │            subscriptions / HTTP 传输）               │
│    └─ zormlib（数据层：DbBase 工厂 + Idb + 五后端 + DbPool）│
└─────────────────────────────────────────────────────────┘
        共享：zjson（JSON 层，mcp-cpp 经 IJsonAdapter 可换）
```

- **mcp-cpp 对 zorm**：上游协议基础设施（类似 ORM 之于驱动）。zorm 只需写"工具定义 + handler 回调"，协议细节全由 mcp-cpp 承担；
- **zorm 对 mcp-cpp**：旗舰集成样例 + 性能/正确性验证床（zjson arena 的实战打磨方）；
- **共同祖先**：zjson（zorm 是作者，mcp-cpp 是重度用户）——这是两者天然协同的根。

### 4.2 结合方式（三层）

| 层 | 做法 | 现状 |
| --- | --- | --- |
| **构建集成** | 根 CMakeLists `add_subdirectory(thirds/mcp-cpp)`；zorm_server 链接 `mcp zormlib` | ✅ 已落地 |
| **工具面** | `db_select/db_insert/db_update/db_delete` 四工具 + JSON Schema 校验；写操作建议后续走 MRTR 确认（`require_confirmation`） | ✅ 已有四工具；确认闸门是下一步 |
| **能力扩展**（建议） | ① 表结构暴露为 Resources（`db://schema/{table}`）；② 表名/字段名补全（`completion/complete`）；③ 连接池化（每请求租用 `DbPool::Lease`，摆脱单连接 g_db）；④ `execSql` 默认禁用的白名单开关 | 未实现，见 §六 路线 |

### 4.3 独立发布与 zorm 的关系

- mcp-cpp 已满足独立发布检查单（§6.2 全勾）：standalone 构建、零警告、无 zorm 符号泄漏、MIT+NOTICE、version.txt；
- **发布时唯一需要剥离的是 `zorm_server.cpp` 示例**（TARGET zormlib 守卫已保证库本体不含）；README 的"zorm 集成"章节作为示例保留。

---

## 五、评审方法论与复现记录

| 步骤 | 做法 | 结果 |
| --- | --- | --- |
| 源码核对 | 逐文件读 include/src 全量（~5000 行）+ 与 schema.md 对照 | 协议无偏差 |
| 独立构建 | `cmake -S . -B build -DMCP_ZJSON_DIR=...` + ninja | 零警告 |
| 全量测试 | ctest 10 项 | 修复前卡死 → **修复后 10/10（1.23s）** |
| 基准复跑 | B1/B2/B3 三件 | 与 README 数字吻合 |
| 根工程集成 | 根 CMakeLists 构建 mcp + zorm_server + mcp_test | 通过（触发修 warning） |

---

## 六、后续路线建议（按价值排序）

1. **zorm_server 增强**：`DbPool` 租用（并发就绪）+ 写操作 MRTR 确认闸门 + 表结构 Resources + `completion` 表/字段补全——让 zorm 集成成为 mcp-cpp 的"旗舰 demo"；
2. **B1 性能优化**：零拷贝 `string_view` 解析（zjson arena 已支持）+ envelope 预扫描快路径，把 0.38×→1×+（兑现 §8 承诺或调低承诺）；
3. **conformance 对齐**：接入 `modelcontextprotocol/conformance` 仓库，冲击 Tier-1 100% 必测项——这是"官方无 C++ SDK"补位的关键认证；
4. **Tasks 扩展 / OAuth 2.1**（v2）：HTTP 远程部署场景需要；
5. **Schema 预编译缓存**（P3 观察项）：B2 派发热点。

---

*评审人：AI 辅助评审（实际构建 + 源码核对）。报告日期 2026-09-22。修复提交：`9b854b3`。*
