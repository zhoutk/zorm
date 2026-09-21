# MCP 协议中文概要

> 面向 zorm 项目改造阅读用。原文基于 [Model Context Protocol 官方规范](https://modelcontextprotocol.io/specification/2026-07-28)（2026-07-28 稳定版）与官方文档（2026-07-28）。
> 原文已下载至本目录 `specification-2026-07-28/` 与 `docs-2026-07-28/`，本文是精简导读，细节请查原文。

---

## 1. MCP 是什么

**Model Context Protocol（MCP）** 是 Anthropic（Claude 所在公司）提出并主导的开源开放协议，用于把 **LLM 应用（Host）** 与 **外部数据源 / 工具（Server）** 标准化地连接起来。官方定位类比：*"AI 应用的 USB-C 接口"*。

核心价值：

- **开发者**：一次实现，接入 Claude、ChatGPT、VS Code Copilot、Cursor 等所有主流 AI 客户端，无需为每家写定制集成。
- **AI 应用/Agent**：获得统一的数据源与工具生态。
- **终端用户**：AI 可以按需访问用户数据并代表用户执行操作（需授权）。

生态现状：Claude Desktop / Claude Code / ChatGPT / VS Code / Cursor / MCPJam 等均已支持；官方 SDK 覆盖 TypeScript、Python、C#、Go、Rust（Tier 1）及 Java、Ruby（Tier 2），**暂无官方 C++ SDK**（社区有第三方实现，如 `modelcontextprotocol/cpp-sdk` 等，属非官方）。

---

## 2. 总体架构

### 2.1 角色

| 角色 | 说明 | 类比 |
| --- | --- | --- |
| **Host** | LLM 应用（桌面 App、IDE、聊天界面），发起连接 | 宿主程序 |
| **Client** | Host 内部的连接器，与 Server 建立 1:1 会话 | 连接驱动 |
| **Server** | 提供上下文与能力的服务，暴露 Resources/Prompts/Tools | 被接入方 |

### 2.2 能力矩阵

**Server 可提供的功能（Features）：**

| 功能 | 说明 | 类比 |
| --- | --- | --- |
| **Resources** | 结构化的上下文/数据（文件、数据库记录、截图等），以 URI 定位，供用户或模型使用 | GET 接口 |
| **Prompts** | 模板化消息与工作流（带参数渲染成消息序列） | 预置 prompt 模板 |
| **Tools** | 模型可调用的函数，执行后返回结构化结果 | POST 接口 / RPC |

**Client 可提供的功能：**

| 功能 | 说明 |
| --- | --- |
| **Sampling** | 服务器发起模型采样（服务端触发的 agentic 行为 / 递归 LLM 调用） |
| **Roots** | 服务器查询客户端可访问的 URI/文件系统边界 |
| **Elicitation** | 服务器向用户请求补充信息（表单等） |

**附加工具：** 配置、进度跟踪（`progress`）、取消（`cancellation`）、错误上报、日志（`logging`）、分页（`pagination`）、参数补全（`completion`）、结果缓存（`caching`，含 `ttlMs`/`cacheScope`）。

### 2.3 分层结构

```
┌─────────────────────────────────────────────┐
│  Server Features (Resources/Prompts/Tools)  │
│  Client Features (Sampling/Roots/Elicitation)│
├─────────────────────────────────────────────┤
│  Message Patterns (请求/响应、MRTR、订阅)      │
│  Versioning & Compatibility                 │
│  Base Protocol (JSON-RPC 2.0)               │
├─────────────────────────────────────────────┤
│  Transports: stdio / Streamable HTTP        │
└─────────────────────────────────────────────┘
```

- **必须实现**：基础协议、版本协商、消息模式。
- **可选实现**：Server/Client Features、Utilities，按需裁剪。

---

## 3. 传输层（Transports）

协议语义在所有传输上一致，传输只是**绑定（binding）**，定义消息如何分帧、投递、携带元数据、取消与终止。

### 3.1 stdio

- 客户端启动子进程，通过其**标准输入/输出**传输消息。
- 消息以**换行符（newline）分隔**，每条消息一行 JSON。
- 适用于本地、单进程场景（Claude Desktop 本地扩展、IDE 插件）。
- 取消：客户端发送 `notifications/cancelled` 通知。

### 3.2 Streamable HTTP

- 每个消息是发往**单一 MCP 端点**的 HTTP POST。
- 响应为 JSON 对象或**请求级 SSE 流**（可用于流式结果）。
- 请求元数据可镜像到 HTTP 头（`MCP-Protocol-Version`、`Mcp-Session-Id` 等），便于中间件路由。
- 适用于远程、Web 场景；鉴权基于 **OAuth 2.1**（授权服务器发现、客户端注册）。
- 取消：关闭请求的响应流。

> 结论：本地工具型 Server 优先 stdio；远程数据服务型 Server 用 Streamable HTTP。两者 JSON 消息层完全一致。

---

## 4. 消息与生命周期

### 4.1 消息类型（JSON-RPC 2.0）

**请求（Request）**：`jsonrpc: "2.0"`、`id`（string|number，不可为 null）、`method`、`params?`。

**响应（Response）**：
- 结果响应：`result` 必须含 `resultType` 字段（`"complete"` = 成功完成；`"input_required"` = 需要补充输入；扩展可加新值；旧协议缺省视为 `"complete"` 以兼容）。
- 错误响应：`error: { code, message, data? }`。
- 错误码：JSON-RPC 标准码 `-32700` / `-32600~-32603`；MCP 保留 `-32020 ~ -32099` 由规范定义（如 `-32002` 等）；`-32000~-32019` 为历史遗留区，新代码禁用。

**通知（Notification）**：无 `id`，不期待响应（如 `notifications/cancelled`、`notifications/progress`、列表变更通知 `notifications/resources/list_changed` 等）。

### 4.2 生命周期（Lifecycle）

```
initialize ──→ 能力协商 ──→ 正常交互（调用 tools/resources/prompts）
   │                                │
   └── initialized 通知             └── 任意时刻：cancelled / ping / 终止
```

- `initialize`：客户端发起，协商协议版本、双方能力、实现信息。
- 所有请求都需携带 `_meta.io.modelcontextprotocol/*` 元数据（`protocolVersion`、`clientInfo`、`clientCapabilities`）。
- 版本协商：双方取支持的**最高共同版本**（当前稳定版 2026-07-28；兼容旧版 2025-11-25、2025-06-18、2025-03-26、2024-11-05）。

### 4.3 关键消息模式（Patterns）

| 模式 | 说明 |
| --- | --- |
| 请求/响应 | 基础 RPC |
| **MRTR（Multi Round-Trip）** | 服务器返回 `input_required`，客户端收集输入后携带 `inputResponses`/`requestState` 重试同一请求 |
| **订阅** | 客户端订阅列表变更，服务器推送通知（`resources/subscribe` 等） |
| **取消** | `notifications/cancelled` 或关闭流 |
| **进度** | `notifications/progress` 上报长时间任务进度 |

---

## 5. 服务器功能详解（重点：与 zorm 相关）

### 5.1 Tools（工具）——zorm 改造的核心

Tools 是**模型可调用**的函数，每个工具由 `name` 唯一标识并声明 `inputSchema`（JSON Schema）。

```jsonc
// tools/list 响应
{
  "tools": [
    {
      "name": "query_orders",
      "title": "订单查询",
      "description": "按条件查询订单表",
      "inputSchema": {
        "type": "object",
        "properties": {
          "table": { "type": "string" },
          "where": { "type": "object" }
        },
        "required": ["table"]
      }
    }
  ],
  "nextCursor": "…",        // 分页游标
  "ttlMs": 300000,          // 缓存 TTL（可选）
  "cacheScope": "public"    // 缓存作用域（可选）
}
```

- 服务器**必须**声明 `tools` capability（可带 `listChanged: true`）。
- `tools/list` 支持分页与缓存；`tools/call` 调用工具，返回 `content: [{type:"text", text:"…"}]` 及 `isError`。
- 安全要求：**必须有真人参与（human-in-the-loop）**，可拒绝工具调用；UI 要明示哪些工具暴露给模型、调用时给视觉提示。

### 5.2 Resources（资源）

- 以 `URI`（`file://`、`db://` 等）定位的上下文数据。
- 方法：`resources/list`、`resources/read`、`resources/templates/list`、`resources/subscribe`（订阅变更）。
- 适合暴露：表结构元数据（schema）、配置、字典数据、查询结果快照。

### 5.3 Prompts（提示词）

- 模板化消息：`name` + `arguments`（带 `required`/`enum` 等约束）→ 渲染成多轮消息序列。
- 方法：`prompts/list`、`prompts/get`。
- 适合暴露：常用查询套路（如"按某条件查用户"）、SQL 生成模板、报表流程。

### 5.4 实用工具（Utilities）

- **分页**：不透明游标（`cursor`/`nextCursor`），服务器定页大小。
- **补全**：`completion/complete` 为参数提供候选项（可用于表名、字段名补全）。
- **日志**：`logging/setLevel`、`notifications/message`。
- **缓存**：`ttlMs` + `cacheScope`（`public`/`session`/`connection`），客户端可缓存 list 结果。
- **取消**：`notifications/cancelled`。
- **进度**：`notifications/progress`。

---

## 6. 客户端功能

- **Sampling（采样）**：服务器请求客户端代为调用 LLM（带 `includeContext` 等控制）；用户必须显式批准。
- **Roots（根）**：客户端声明可访问的文件/URI 边界，服务器可 `roots/list` 查询。
- **Elicitation（信息征集）**：服务器通过 `elicitation/create` 向用户请求补充信息（表单模式、URL 模式等），配合 MRTR 使用。

---

## 7. 授权与安全

- 传输层鉴权基于 **OAuth 2.1**：授权服务器发现（`/.well-known/oauth-protected-resource`）、动态客户端注册、PKCE、`Mcp-Session-Id` 会话管理。
- **安全核心原则**（不可由协议强制，需实现者落实）：
  1. 用户同意与控制（所有数据访问/操作需显式同意）；
  2. 数据隐私（不得未经同意外传用户数据）；
  3. 工具安全（工具=任意代码执行，描述不可信，调用需用户确认）；
  4. LLM 采样控制（采样请求需用户批准，协议刻意限制服务器对 prompt 的可见性）。

---

## 8. 版本与兼容

- 当前稳定版：**2026-07-28**（文档/规范），此前有 2025-11-25、2025-06-18、2025-03-26、2024-11-05，另有 draft。
- 版本协商取双方最高共同版本；服务器实现旧版时客户端须兼容缺省 `resultType` 等。
- 官方 conformance 测试仓库：`modelcontextprotocol/conformance`（SDK 分级依据，Tier 1 需 100% 通过）。

---

## 9. 与 zorm 的结合点（速览）

| zorm 能力 | 对应 MCP 概念 | 实现方式 |
| --- | --- | --- |
| `Idb::select/querySql`（查询） | **Tool**（或 Resource 快照） | `tools/call` → 生成 SQL → 返回 JSON |
| `Idb::create/update/remove/execSql`（写操作） | **Tool**（需 human-in-the-loop 确认） | `tools/call` + `isError` |
| `Idb::insertBatch/transGo`（事务） | **Tool**（MRTR 收集确认） | `tools/call` |
| 表结构/字段元数据（`DbUtils`） | **Resource**（`db://schema/{table}`） | `resources/read` |
| 字段名/表名补全 | **Completion** | `completion/complete` |
| 常见查询套路 | **Prompt** | `prompts/get` |
| 分页查询 | **Pagination**（游标） | `tools/list`/`resources/list` 游标 |

> 注意 zorm 数据以 **Json（zjson）** 进出 —— MCP 消息本身就是 JSON，zjson 可直接复用做消息编解码（需注意 zjson 的 JSON 嗅探约定：文本值需 `Json::str()` 包裹）。

---

*文档生成日期：2026-09-21。原文目录：`./specification-2026-07-28/`、`./docs-2026-07-28/`。*
