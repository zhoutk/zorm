# 将 zorm 改造成 MCP Server —— 可行性评估与改造计划

> 关联文档：`./mcp-summary.md`（协议中文概要）、`./specification-2026-07-28/`（协议原文）、`./docs-2026-07-28/`（官方文档）。
> 评估日期：2026-09-21。

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
| **官方无 C++ SDK** | 官方 Tier-1 SDK 只有 TS/Python/C#/Go/Rust，**没有 C++**。两条路线：① 自研最小协议栈（JSON-RPC 2.0 + stdio/HTTP 分帧，工作量约 1~2k 行，zjson 已解决 JSON 层）；② 复用社区 C++ 实现（需评估维护风险）。**建议路线①**，因为 MCP 服务端所需子集很小 |
| **写操作安全** | Tools 代表任意代码执行。MCP 要求 human-in-the-loop。设计上写工具（create/update/remove/execSql/transGo）**默认需确认**，利用 MCP 的 MRTR/`input_required` 机制实现"用户确认后执行" |
| **SQL 注入/越权** | 工具入参走 zorm 参数化路径（`parameterized`），禁止把 `execSql` 裸 SQL 暴露成通用工具，或仅对可信客户端开放 |
| **五库方言差异** | 全部走 `Idb` 统一接口，由 zorm 现有方言层消化，MCP 层不感知方言 |
| **协议版本演进** | 当前稳定版 2026-07-28。只实现核心 subset，按官方 conformance 测试校准 |

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

### 2.2 核心决策

| 决策点 | 选择 | 理由 |
| --- | --- | --- |
| 实现语言 | **C++17，复用 zormlib** | 核心库不动；避免再维护一套 TS/Python 版本 |
| 传输 | **首期 stdio，二期 Streamable HTTP** | stdio 覆盖本地 AI 客户端主流场景，实现最简单；HTTP 用于远程部署 |
| 服务器能力 | **Tools（必需）+ Resources（表结构）+ Prompts（查询套路）** | Tools 是刚需；Resources/Prompts 增量成本低、体验提升大 |
| 客户端能力 | 首期不实现（Sampling/Roots/Elicitation 全部不声明） | 裁剪协议面，降低实现量 |
| 连接管理 | 复用 `DbPool::HandlePool` | 每会话一个 pool，多客户端并发安全 |
| JSON 层 | 复用 zjson | 已含解析/序列化；注意 `Json::str()` 嗅探约定 |

---

## 三、改造计划（分阶段）

### 阶段 0：协议验证（0.5~1 周）

- [ ] 通读 `docs/mcp/mcp-summary.md` + 原文 `basic/index.md`、`server/tools.md`、`basic/transports/stdio.md`。
- [ ] 用官方 [MCP Inspector](https://modelcontextprotocol.io/docs/2026-07-28/tools/inspector.md) 跑通一个"hello world"C++ server（手写 JSON-RPC over stdio），验证工具发现/调用闭环。
- [ ] 决策：自研协议层 vs 引入社区 C++ SDK（写个对比小文档，记录取舍）。

**产出**：最小可运行的 C++ MCP Server 骨架 + 技术选型结论。

### 阶段 1：协议层（1~2 周）

新增 `src/mcp/` 目录（私有层，不进 public include）：

- [ ] **JSON-RPC 2.0 消息编解码**（`McpMessage.h/.cpp`）：Request/Response/Notification/Error 结构体，基于 zjson 序列化；严格校验 `resultType`、错误码规范（-32700/-32600~32603/-32020~32099）。
- [ ] **stdio 传输绑定**（`McpStdioTransport.h/.cpp`）：换行分隔 JSON 帧读写 stdin/stdout；处理 `notifications/cancelled`。
- [ ] **生命周期**（`McpServer.h/.cpp`）：`initialize`（版本协商：声明 2026-07-28，接受旧版）、`initialized` 通知、`ping`；能力声明（`capabilities.tools.listChanged`）。
- [ ] **元数据**：所有请求携带 `_meta.io.modelcontextprotocol/*`（protocolVersion/clientInfo/clientCapabilities）。
- [ ] **错误处理**：`McpError` 统一错误码映射（未实现方法 → `-32601 Method not found` 等）。

**产出**：独立可测的 MCP 协议层（可用 `tests/test_mcp_protocol.cpp` 单测消息编解码）。

### 阶段 2：工具适配层（1~2 周）

- [ ] **工具注册表**（`McpToolRegistry.h`）：声明式定义工具名、描述、inputSchema（JSON Schema）。
- [ ] **Idb 适配工具集**（核心 8 个工具 + 辅助）：

| 工具名 | 对应 Idb | 说明 |
| --- | --- | --- |
| `db_select` | `select()` | 按表名+条件查询，支持 fields/values/分页 |
| `db_create` | `create()` | 插入记录（写操作） |
| `db_update` | `update()` | 更新记录（写操作） |
| `db_remove` | `remove()` | 删除记录（写操作，高危） |
| `db_query_sql` | `querySql()` | 参数化 SQL 查询（**白名单/只读**） |
| `db_exec_sql` | `execSql()` | 参数化 SQL 执行（**默认禁用，配置开启**） |
| `db_insert_batch` | `insertBatch()` | 批量插入 |
| `db_trans_go` | `transGo()` | 事务（JSON 数组多条 SQL） |

- [ ] **JSON Schema 生成**：为每个工具生成 `inputSchema`（属性、required、description——描述要面向 LLM）。
- [ ] **结果封装**：`{resultType:"complete", content:[{type:"text", text: json}]}`、错误 → `isError:true`。
- [ ] **安全闸门**：写工具（create/update/remove/exec_sql/trans_go）在配置 `require_confirmation=true`（默认）时返回 `input_required`（MRTR），要求用户确认后再执行。
- [ ] **方言透明**：工具层只面对 `Idb*`，后端选择由启动参数（`--dbtype sqlite3|mysql|...` + options JSON）决定。

**产出**：可被 Claude Code / VS Code 实际接入的 zorm MCP Server（stdio），五后端可切换。

### 阶段 3：Resources / Prompts / 补全（1 周）

- [ ] **Resources**：`db://schema`（库表清单）、`db://schema/{table}`（表结构：列、类型、主键）——基于 `DbUtils`/各库元数据查询；支持 `resources/list`、`resources/read`。
- [ ] **Prompts**：预置查询模板（如"按条件查用户"、"导出报表"），`prompts/list`、`prompts/get`。
- [ ] **Completion**：`completion/complete` 为工具参数提供表名、字段名候选。
- [ ] **Pagination**：`db_select` 等 list 场景支持游标分页（复用 zorm 的 page/size 保留字）。

**产出**：功能完整版（Tools+Resources+Prompts+Completion）。

### 阶段 4：Streamable HTTP 传输 + 安全硬化（1~2 周，可选）

- [ ] **HTTP 传输**：单端点 POST + 请求级 SSE 流；`Mcp-Session-Id` 会话头；可选 OAuth 2.1（授权发现/客户端注册）——本地工具场景可先不做 OAuth。
- [ ] **安全硬化**：参照 `security_best_practices.md`：写操作确认 UI 提示、SQL 白名单、连接串不落日志、超时/取消、并发连接上限。
- [ ] **配置化**：`mcp-server.json`（或命令行）：dbtype、连接 options、工具开关（禁用危险工具）、确认策略、只读模式。

### 阶段 5：测试与发布（1 周）

- [ ] **协议单测**：消息编解码、错误码、生命周期状态机（`tests/test_mcp_protocol.cpp`）。
- [ ] **端到端测试**：用 MCP Inspector + 真实 AI 客户端（Claude Code/VS Code）对五后端各跑一遍工具调用闭环。
- [ ] **复用回归门**：核心库零改动 → `./run-test all` 必须全绿（改造不改 core 的硬约束）。
- [ ] **文档**：`docs/mcp/README.md`（使用说明：如何注册到 Claude Code / VS Code / Cursor）、`docs/mcp-server-design.md`（设计文档，补充本计划细节）。
- [ ] **发布**：可选——打包二进制，或提交到 MCP Registry（官方注册表支持，见 `registry/` 文档）。

---

## 四、工作量估算

| 阶段 | 内容 | 估时 | 关键产出 |
| --- | --- | --- | --- |
| 0 | 协议验证/选型 | 0.5~1 周 | 骨架 + 选型结论 |
| 1 | 协议层 | 1~2 周 | `src/mcp/` + 单测 |
| 2 | 工具适配层 | 1~2 周 | 可用 MCP Server（stdio） |
| 3 | Resources/Prompts/补全 | 1 周 | 功能完整版 |
| 4 | HTTP + 安全 | 1~2 周（可选） | 远程部署版 |
| 5 | 测试与发布 | 1 周 | 文档 + 回归全绿 |

**合计：约 6~10 人周**（若跳过阶段 4，约 5~8 人周）。核心库零改动，风险集中在新增的 `src/mcp/` 层。

---

## 五、落地建议（立即行动）

1. **先做阶段 0 的选型结论**：写 1~2 页对比"自研 vs 社区 C++ SDK"，再定技术路线——这是唯一有不确定性的决策。
2. **阶段 2 完成后立刻接入 Claude Code 试用**：体验"用自然语言查 mysql 库"的闭环，验证产品价值，再决定是否投入阶段 3~5。
3. **坚持"核心零改动"硬约束**：所有 MCP 逻辑在 `src/mcp/` 新层，`Idb.h` 签名冻结不动，`./run-test all` 作为回归门。
4. **安全默认从严**：写工具默认确认、`execSql` 默认禁用、只读模式一键开启。

---

*本文档由协议概要 + 官方规范原文整理而成。*
