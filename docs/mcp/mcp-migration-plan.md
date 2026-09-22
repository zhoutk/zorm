# 将 zorm 改造成 MCP Server —— 可行性评估与改造计划

> 关联文档：`./mcp-summary.md`（协议中文概要）、`./specification-2026-07-28/`（协议原文）、`./docs-2026-07-28/`（官方文档）、`./mcp-cpp-review.md`（mcp-cpp 评审报告）。
> 评估日期：2026-09-21 · **2026-09-22 更新**：协议层已由 `thirds/mcp-cpp` 独立库落地（M0–M5 完成），本文阶段 0–5 相应标记进度并修订后续路线。

---

## 〇、重大进展（2026-09-22）

原计划"自研协议层"已升级为 **`thirds/mcp-cpp` 独立 C++ MCP SDK**（可独立发布），M0–M5 全部完成并通过评审（见 `./mcp-cpp-review.md`）。**当前 zorm 侧的 MCP Server 可直接基于 mcp-cpp 构建，无需再从零写协议层。**

| 原计划项 | 现状 |
| --- | --- |
| 阶段 0 协议验证/选型 | ✅ 完成（mcp-cpp 即选型结论：自研优于社区 SDK） |
| 阶段 1 协议层 | ✅ 完成（mcp-cpp 提供，远超原计划子集：含 HTTP/SSE/订阅/校验器） |
| 阶段 2 工具适配层 | ✅ 完成（`examples/zorm_server.cpp` 四工具 + JSON Schema） |
| 阶段 3 Resources/Prompts/补全 | ⏳ 部分（mcp-cpp 能力就绪，zorm 侧表结构 Resource/补全待接线） |
| 阶段 4 HTTP + 安全 | ✅ 完成（mcp-cpp Streamable HTTP + 头镜像 + Origin 防护；OAuth 为 v2） |
| 阶段 5 测试与发布 | ⏳ mcp-cpp 侧完成（10/10 测试 + 独立发布检查单）；zorm 集成 e2e/发布待做 |

---

## 一、这个想法怎么样？—— 结论：**非常值得做，方向正确**

将 zorm 改造成 MCP Server，本质是把一个"给程序调用的 ORM 库"升级为"**给 AI 调用的数据接口层**"。这是一个高价值、低耦合、有明确生态红利的改造。

### 1.1 为什么值得做

| 维度 | 分析 |
| --- | --- |
| **生态红利** | MCP 是当前 AI 应用接入事实标准（Claude/Claude Code/ChatGPT/VS Code Copilot/Cursor 全支持）。改造一次，五类后端（sqlite3/mysql/postgres/dm8/jsonfile）**统一暴露给所有 AI 客户端**，无需为每家写定制集成 |
| **架构契合度极高** | zorm 的 `Idb` 接口（`select/create/update/remove/querySql/execSql/insertBatch/transGo`）本身就是**天然的"工具集"定义**——每条接口恰好对应一个 MCP Tool。改造不需要动核心 |
| **数据形态天然匹配** | zorm 数据以 `Json`（zjson）进出，MCP 消息本身就是 JSON。JSON 编解码层可直接复用 zjson（注意其嗅探约定） |
| **差异化价值** | 市面 MCP 数据库服务器大多是"SQL 直通"（暴露裸 SQL）。zorm 的 **ORM 语义层**（智能查询、分页、方言适配、事务封装、五库统一）是更高层的抽象，正好是 **MCP 的 Tools 层该有的样子**——模型不需要懂各库方言 |
| **成本可控** | 协议层可裁剪（只实现 Tools + Resources 两个 server feature + stdio 传输即可跑通 90% 场景），核心库不动，回归门（`./run-test all`）可保障 |
| **参考先例** | 官方/社区有多个"ORM 套 MCP"案例（如 Prisma 生态的 MCP、TypeORM MCP），验证了该模式可行 |

### 1.2 主要风险与对策

| 风险 | 对策 |
| --- | --- |
| **官方无 C++ SDK** | 官方 Tier-1 SDK 只有 TS/Python/C#/Go/Rust，**没有 C++**。**已解决**：自研路线①落地为 `thirds/mcp-cpp` 独立库（Tier-1 功能对齐 + 可独立发布，见评审报告），zorm 侧零协议成本 |
| **写操作安全** | Tools 代表任意代码执行。MCP 要求 human-in-the-loop。设计上写工具（insert/update/delete）**默认需确认**，利用 MCP 的 MRTR/`input_required` 机制实现"用户确认后执行"（mcp-cpp 已支持 MRTR，zorm_server 接线待办） |
| **SQL 注入/越权** | 工具入参走 zorm 参数化路径（`parameterized`），禁止把裸 SQL 暴露成通用工具，或仅对可信客户端开放（zorm_server 暂未暴露 querySql/execSql，天然规避） |
| **五库方言差异** | 全部走 `Idb` 统一接口，由 zorm 现有方言层消化，MCP 层不感知方言（已验证：zorm_server 通过 dbType 参数切换后端） |
| **协议版本演进** | 当前稳定版 2026-07-28。mcp-cpp 支持五版协商（2024-11-05 → 2026-07-28），`-32022` 版本协商错误码，按 conformance 校准 |

---

## 二、改造方案总览

### 2.1 目标形态

```
┌──────────────────────────────────────────────────────┐
│  AI 客户端 (Claude Code / VS Code / Cursor / ...)      │
│    └─ MCP Client                                       │
└───────────────┬──────────────────────────────────────┘
                │  stdio 或 Streamable HTTP（JSON-RPC 2.0）
┌───────────────▼──────────────────────────────────────┐
│  zorm-mcp-server（新可执行程序，C++17，复用 zormlib）    │
│                                                       │
│  ┌─────────────────────────────────────────────┐      │
│  │ MCP 协议层（新）:                            │      │
│  │  · JSON-RPC 2.0 编解码（基于 zjson）          │      │
│  │  · 生命周期 initialize/initialized           │      │
│  │  · 传输绑定: stdio（首期）→ HTTP（二期）       │      │
│  │  · Tools/Resources/Prompts 能力              │      │
│  ├─────────────────────────────────────────────┤      │
│  │ 适配层（新）: MCP 工具 ↔ Idb 调用映射          │      │
│  │  · 工具注册表 + 入参 JSON Schema 声明          │      │
│  │  · 结果封装 {content, isError}               │      │
│  ├─────────────────────────────────────────────┤      │
│  │ zorm 核心（不变）:                            │      │
│  │  · DbBase 工厂 + Idb 接口                     │      │
│  │  · SqlBackendBase + 五后端                    │      │
│  │  · DbPool / DbUtils                           │      │
│  └─────────────────────────────────────────────┘      │
└──────────────────────────────────────────────────────┘
```

### 2.2 核心决策（2026-09-22 修订）

| 决策点 | 选择 | 理由 |
| --- | --- | --- |
| 实现语言 | **C++17，复用 zormlib** | 核心库不动；避免再维护一套 TS/Python 版本 |
| 协议层 | **`thirds/mcp-cpp` 独立库**（原"自研 src/mcp"） | 已 M0–M5 完成、独立可发布、Tier-1 功能对齐（见评审报告）；zorm 只需写工具定义 |
| 传输 | **stdio + Streamable HTTP 均已就绪**（mcp-cpp） | 本地 stdio 为主，HTTP 已具备（含 SSE 订阅） |
| 服务器能力 | **Tools（四工具已实现）+ Resources（表结构，待接线）+ Prompts（待定）** | Tools 是刚需已落地；Resources 增量小、体验提升大 |
| 客户端能力 | 首期不实现（Sampling/Roots 已弃用；Elicitation 走 MRTR） | 裁剪协议面，mcp-cpp 按 SEP-2577 有意不做 Roots/Sampling |
| 连接管理 | 复用 `DbPool::HandlePool` | 当前 zorm_server 用单连接 `g_db`；并发场景改为每请求租用 |
| JSON 层 | 复用 zjson（mcp-cpp 经 IJsonAdapter 注入） | 已含解析/序列化；注意 `Json::str()` 嗅探约定 |

---

## 三、改造计划（分阶段，2026-09-22 修订）

### 阶段 0：协议验证/选型 —— ✅ 完成（mcp-cpp 即产出）

- [x] 通读 `docs/mcp/mcp-summary.md` + 原文协议章节。
- [x] 选型决策：**自研 mcp-cpp 独立库**（优于社区 C++ SDK：官方无 C++、自研可控、可独立发布）。
- [x] 用官方 MCP Inspector 跑通最小 server（`examples/minimal_server`）。

### 阶段 1：协议层 —— ✅ 完成（由 `thirds/mcp-cpp` 承担，超出原计划子集）

原计划手写 `src/mcp/` 的 JSON-RPC 编解码/stdio/生命周期/元数据/错误处理，已由 mcp-cpp 全面实现且**更完整**：

- [x] JSON-RPC 2.0 MCP 化编解码（`jsonrpc.hpp/.cpp`）——id 禁 null、无批量、严格校验；
- [x] stdio 传输（`transport.hpp`）——换行分帧、Windows 二进制模式、EOF 优雅退出；
- [x] 2026 无状态化：per-request `_meta`（protocolVersion/clientCapabilities）——`types.hpp`；
- [x] `server/discover`、`subscriptions/listen`、分页、CacheableResult、错误码分区——`server.hpp/.cpp`；
- [x] Streamable HTTP + SSE + 头镜像 + Origin 防护——`transport_http.hpp/.cpp`；
- [x] JSON Schema 2020-12 校验器（资源上限防护）——`schema.hpp/.cpp`；
- [x] Client API（`client.hpp/.cpp`）+ MRTR（input_required 重试）。

### 阶段 2：工具适配层 —— ✅ 完成（`examples/zorm_server.cpp`，四工具）

- [x] 工具注册表 + inputSchema（JSON Schema）——mcp-cpp `Builder::simpleTool` + `schema()`；
- [x] zorm 工具集：`db_select` / `db_insert` / `db_update` / `db_delete`（四工具，真库冒烟通过）；
- [x] 结果封装 `{resultType, content:[{type:text,text}]}`；
- [x] 方言透明：启动参数 `zorm_server <dbType> [options]` 切换后端。

**待办（写操作安全闸门）**：写工具（insert/update/delete）的 MRTR 确认——`require_confirmation` 配置返回 `input_required` 要求用户确认后执行；`db_query_sql/db_exec_sql/insert_batch/trans_go` 未暴露，可后续按需加。

### 阶段 3：Resources / Prompts / 补全（剩余接线）

mcp-cpp 能力已就绪（resources/prompts/completion 注册表 + 分页游标），zorm 侧待接线：

- [ ] **Resources**：`db://schema`（库表清单）、`db://schema/{table}`（表结构：列、类型、主键）——基于 `DbUtils`/各库元数据查询；
- [ ] **Completion**：`completion/complete` 为工具参数提供表名、字段名候选（mcp-cpp 已支持 handler）；
- [ ] **Prompts**（可选）：预置查询模板；
- [ ] **分页**：`db_select` 复用 zorm page/size 保留字，接 mcp-cpp 游标。

### 阶段 4：Streamable HTTP + 安全硬化 —— ✅ 协议侧完成，zorm 集成侧待配

- [x] mcp-cpp Streamable HTTP：单端点 POST、SSE 订阅流、头镜像、404/405、Origin 防护（评审已验证）；
- [ ] zorm_server 增加 HTTP 启动模式（`--http <port>`）与配置化（工具开关、只读模式、确认策略）；
- [ ] OAuth 2.1（v2，mcp-cpp 未做，远程公网部署需要）。

### 阶段 5：测试与发布（剩余部分）

- [x] mcp-cpp 测试：10/10 CTest + 独立发布检查单全勾（见评审报告）；
- [ ] **zorm 集成 e2e**：用 MCP Inspector + 真实 AI 客户端（Claude Code/VS Code）对五后端各跑一遍工具闭环；
- [ ] **回归门**：核心库零改动 → `./run-test all` 全绿（硬约束）；
- [ ] **文档**：`docs/mcp/README.md`（注册到 Claude Code / VS Code / Cursor 的使用说明）；
- [ ] **发布**（可选）：mcp-cpp 独立发布（`git subtree split`）或提交 MCP Registry。

---

## 四、工作量估算（2026-09-22 修订）

| 阶段 | 内容 | 估时 | 状态 |
| --- | --- | --- | --- |
| 0 | 协议验证/选型 | 0.5~1 周 | ✅ 完成（mcp-cpp 产出） |
| 1 | 协议层 | 1~2 周 | ✅ 完成（mcp-cpp M0–M1） |
| 2 | 工具适配层 | 1~2 周 | ✅ 完成（zorm_server 四工具） |
| 3 | Resources/Prompts/补全 | 1 周 | ⏳ 待接线（mcp-cpp 就绪） |
| 4 | HTTP + 安全 | 1~2 周（可选） | ⏳ 协议侧完成，zorm 集成侧待配 |
| 5 | 测试与发布 | 1 周 | ⏳ zorm 集成 e2e/发布待做 |

**剩余工作量：约 1~2 周**（阶段 3 接线 + 阶段 5 zorm 集成 e2e；阶段 4 HTTP 模式与安全配置可选）。核心库零改动约束保持。

---

## 五、落地建议（2026-09-22 修订）

1. **基于 mcp-cpp 直接构建**：协议层已就绪，zorm 侧聚焦"工具面 + 安全闸门 + 接线"。
2. **先做写操作 MRTR 确认闸门**（阶段 2 待办）：安全默认从严，写工具需用户确认。
3. **阶段 3 接线**：表结构 Resources + completion 补全，体验提升大、成本低。
4. **坚持"核心零改动"硬约束**：`Idb.h` 签名冻结，`./run-test all` 回归门。
5. **zorm_server 并发化**：从单连接 `g_db` 改为 `DbPool::Lease` 每请求租用（评审 P3 建议）。

---

*本文档由协议概要 + 官方规范原文 + mcp-cpp 评审报告整理而成。*
