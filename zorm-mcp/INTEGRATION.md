# zorm-mcp 接入指南（AI 客户端全配置手册）

> 一个 server，五种后端（sqlite3 / jsonfile / mysql / postgres / dm8），13 个结构化数据库工具。
> 本文档给出**每一个主流 AI 客户端**的具体接入配置；已验证的给出实测配置，未验证/未支持的明确标注占位。
>
> 前置阅读：[README.md](README.md)（工具一览 / MRTR 确认机制 / 常见问题）。

---

## 0. 通用前提（所有客户端共用）

### 0.1 构建（一次性）

```bash
cmake --preset clang-rlz && cmake --build build --target zorm_mcp_server
# 产物: build/zorm-mcp/zorm_mcp_server.exe（运行 DLL 已自动复制到同目录）
```

构建时 POST_BUILD 会解析 exe 的**完整依赖闭包**并复制到同目录：不只 `sqlite3.dll`/`libpq.dll`/`dmdpi.dll` 这些后端 DLL，还有 `libc++.dll`、`libmariadb.dll` 及它们的传递依赖（来自 `D:/msys64/clang64/bin`）。AI 客户端用**自己的环境**孵化 server（没有 MSYS2 PATH），所以这一步是"免配 PATH 直接接"的前提——否则子进程加载期即退出，客户端只看到 `Process exited with code 3221225781`（0xC0000135 `STATUS_DLL_NOT_FOUND`）。

### 0.2 接入命令用哪个：**统一用 `start.sh` 包装脚本（推荐，MSYS2 标准示范）**

AI 客户端各自决定子进程的工作目录（cwd），**MCP 协议对此没有任何约定**——同一个注册配置，Claude Code、Claude Desktop、Cursor 各自 spawn 出来的 cwd 都不一样。zorm-mcp 不带 `--config` 时，默认数据库文件（`zorm-mcp.db` / `zorm-mcp.json`）落在**进程 cwd**，所以"库到底在哪"会随客户端漂移。包装脚本把 cwd 钉死到 `zorm-mcp/` 目录，文件位置从此可预期：

| 形式 | 命令 | 适用 |
| --- | --- | --- |
| **包装脚本（推荐，标准示范）** | `D:/codes/zorm/zorm-mcp/start.sh`（MSYS2/Git Bash） | 免配 cwd，默认库落在 `zorm-mcp/` 目录 |
| 备选 | `D:/codes/zorm/zorm-mcp/start.cmd`（cmd 批处理，行为等价） | 客户端无法直接执行 .sh 时 |
| 直接指 exe | `D:/codes/zorm/build/zorm-mcp/zorm_mcp_server.exe` | 客户端支持配 cwd，或你用 `--config` 给绝对路径 |

传参直接跟在后面，例如 `start.sh --allow-sql --config D:/data/zorm.json`。

**常见疑问：为什么 demo 时库在项目目录，接 MCP 后就不在了？**
demo 是在项目目录里手工启动 server 的，库文件自然落在当时的 cwd；接 MCP 后 cwd 由客户端决定（且被 start.sh 钉到 `zorm-mcp/`），这是**设计行为，不是 bug**。依赖客户端 cwd 本身就不可靠，通行做法是**不依赖 cwd，用 `--config <绝对路径>` 显式指定数据库**——库的位置、命名、后端全部由你决定，与哪个客户端接入无关。

**注意相对路径的解析基准**：`--config` 里的相对 `connString` 是相对**服务器进程 cwd**（被 start.sh 钉到 `zorm-mcp/`）解析的，不是相对你运行 AI 客户端的项目目录。要让库落在某个项目里，`connString` 写**绝对路径**指向它。

### 0.3 多后端（mysql / postgres / dm8）

写一份配置（模板见 [`zorm-mcp.json`](zorm-mcp.json)，含五后端示例），启动参数加 `--config <绝对路径>`：

```jsonc
{ "databases": {
    "sqlite3":  { "type": "sqlite3",  "options": { "connString": "D:/data/app.db" } },
    "jsonfile": { "type": "jsonfile", "options": { "connString": "D:/data/app.json" } },
    "mysql":    { "type": "mysql",    "options": { "db_host": "...", "db_user": "...", "db_pass": "...", "db_name": "...", "db_port": 3306 } },
    "postgres": { "type": "postgres", "options": { "db_host": "...", "db_user": "...", "db_pass": "...", "db_name": "...", "db_port": 5432 } },
    "dm8":      { "type": "dm8",      "options": { "db_host": "...", "db_user": "...", "db_pass": "..." } }
} }
```

`db` 路由参数 = 配置里的**键名**。`options` 里无需写 `DbLogClose`（stdio 模式强制静默日志）；`parameterized` 缺省按 true 处理。

### 0.4 写确认（MRTR）与两个开关

- 所有写工具（insert / insert_batch / update / delete / trans / exec_sql）**首次调用不执行**：返回 `input_required` + `requestState`，确认文案含目标库/表/影响行数。AI 会把确认请求转述给你，批准后自动带 `requestState + inputResponses` 重试落盘。
- `--yes` 关闭确认（自动化/测试专用）；`--allow-sql` 才会注册 `db_exec_sql`（任意写/DDL SQL）。
- 客户端若支持 elicitation 会弹原生确认框；不支持时由 AI 转述，效果等价。

### 0.5 接入后 30 秒自检

对 AI 说：**"用 zorm-mcp 看看 sqlite3 里有哪些表，描述一下其中一张表的结构"**。应看到 `db_list_tables` → `db_describe_table` 两次工具调用（空库会返回空表清单，属正常）。

---

## 1. Claude Code（已验证 ✅）

```bash
# 最简接入：内置 sqlite3 + jsonfile，默认库文件落在 zorm-mcp/ 目录
claude mcp add zorm-mcp -- D:/codes/zorm/zorm-mcp/start.sh

# 推荐：加 --config 显式指定数据库——位置/后端由配置决定，不随 cwd 漂移
claude mcp add zorm-mcp -- D:/codes/zorm/zorm-mcp/start.sh --config D:/codes/zorm/zorm-mcp/zorm-mcp.json
```

项目级共享配置（签入仓库，团队共用）——在仓库根建 `.mcp.json`：

```json
{ "mcpServers": { "zorm-mcp": {
    "command": "D:/codes/zorm/zorm-mcp/start.sh",
    "args": ["--config", "D:/codes/zorm/zorm-mcp/zorm-mcp.json"]
} } }
```

> **建议始终带 `--config`**：一是数据库位置显式可查（出问题第一眼就知道在操作哪个库文件）；二是同一个 server 可以按项目换不同的配置文件（各项目的 `.mcp.json` 指向各自的 config 即可）。模板见 [`zorm-mcp.json`](zorm-mcp.json)，`connString` 一律写绝对路径（见 §0.2 的相对路径说明）。

要点：
- 服务器的 `instructions` 决策表会注入模型上下文，工具选择无需人工提示；
- 写操作确认以 `input_required` 形式回到会话，Claude Code 转述给用户；
- 若提示缺 DLL：确认 exe 同目录有 `sqlite3.dll` **以及 `libc++.dll`/`libmariadb.dll`**（构建时按依赖闭包自动复制；只重建 `zorm_mcp_server` 目标即可补齐）。

## 2. Claude Desktop（配置方式标准，同族通用）

`claude_desktop_config.json`（设置 → Developer → Edit Config）：

```json
{ "mcpServers": { "zorm-mcp": { "command": "D:/codes/zorm/zorm-mcp/start.sh" } } }
```

## 3. DeepSeek Harness（dsh，已验证 ✅ 严格客户端）

DSH 使用 MCP SDK 2.0（协议 2026-07-28），对 `tools/list` 等结果的 `ttlMs`/`cacheScope` 严格校验——mcp-cpp 已默认注入，直接可用。

编辑 `C:\Users\<user>\.dsh\profiles\web\cordis.patch.yml`，数组末尾**追加**：

```yaml
# zorm-mcp (C++ stdio server, sqlite3 + jsonfile 开箱, mysql/pg/dm8 加配置)
- insert:
    - id: mcp-zorm-mcp
      name: '@deepseek-ai/dsh-mcp-client'
      config:
        serverName: zorm-mcp
        transport: stdio
        command: D:/codes/zorm/zorm-mcp/start.sh
        cwd: D:/codes/zorm/zorm-mcp
```

保存即热加载（`patchReload: "live"`）。日志出现 `mcp-client(zorm-mcp)` 即注册成功；见 `server is disconnected` 先确认 exe 是最新构建。工具名形如 `mcp__zorm-mcp__db_query`。

## 4. VS Code / GitHub Copilot（同一份配置）

VS Code 1.99+ 原生 MCP，Copilot Chat 的 agent 模式直接消费同一配置。

**项目级** `.vscode/mcp.json`（推荐，可签入）：

```json
{ "servers": { "zorm-mcp": { "command": "D:/codes/zorm/zorm-mcp/start.sh" } } }
```

**用户级**：`settings.json` 里同结构的 `"mcp": { "servers": { ... } }`。保存后 Copilot/Agent 模式的工具列表出现 `db_*` 即成功。JetBrains 全家桶（IDEA/PyCharm 2025.2+）在 Settings → Tools → MCP 里加 stdio server，command 同上。

## 5. Cursor

`~/.cursor/mcp.json`（全局）或项目根 `.cursor/mcp.json`：

```json
{ "mcpServers": { "zorm-mcp": { "command": "D:/codes/zorm/zorm-mcp/start.sh" } } }
```

保存后 Cursor Settings → MCP 显示绿点即成功。

## 6. Windsurf（Codeium）

`~/.codeium/windsurf/mcp_config.json`：

```json
{ "mcpServers": { "zorm-mcp": { "command": "D:/codes/zorm/zorm-mcp/start.sh" } } }
```

## 7. Cline / Roo Code（VS Code 插件）

Cline：侧栏 MCP Servers 图标 → Configure → `cline_mcp_settings.json`（Roo Code 同构，文件名 `mcp_settings.json`）：

```json
{ "mcpServers": { "zorm-mcp": {
    "command": "D:/codes/zorm/zorm-mcp/start.sh",
    "args": [],
    "disabled": false,
    "autoApprove": ["db_list_tables", "db_describe_table", "db_indexes", "db_info",
                     "db_query", "db_count", "db_query_sql"]
} } }
```

`autoApprove` 只放行**只读**工具；写工具保持每次确认（与 MRTR 双保险）。

## 8. Codex CLI（OpenAI）

`~/.codex/config.toml`：

```toml
[mcp_servers.zorm-mcp]
command = "D:/codes/zorm/zorm-mcp/start.sh"
args = []
```

Codex 会话内工具以 `zorm-mcp.db_query` 形态暴露。

## 9. Gemini CLI（Google）

`~/.gemini/settings.json`：

```json
{ "mcpServers": { "zorm-mcp": { "command": "D:/codes/zorm/zorm-mcp/start.sh", "args": [] } } }
```

## 10. MCP Inspector（协议调试，已验证 ✅）

```bash
# stdio 模式
npx @modelcontextprotocol/inspector D:/codes/zorm/build/zorm-mcp/zorm_mcp_server.exe
# HTTP 模式（transport 选 Streamable HTTP，URL 填 http://127.0.0.1:18099/mcp）
D:/codes/zorm/build/zorm-mcp/zorm_mcp_server.exe --http 18099 --yes &
npx @modelcontextprotocol/inspector --transport http http://127.0.0.1:18099/mcp
```

UI 里可逐个调工具、看 resources / prompts（`db-playbook`）/ MRTR 确认流。

## 11. 通用 HTTP 接入（远程 / 自研 Agent）

`--http <port>` 起纯 Streamable HTTP 服务（不跑 stdio），端点 `http://127.0.0.1:<port>/mcp`，多 worker / 浏览器 Inspector / 远程客户端均可接：

```bash
D:/codes/zorm/build/zorm-mcp/zorm_mcp_server.exe --http 18099
```

裸 curl 冒烟（新协议头镜像校验：`Mcp-Name` 必须等于 body 里的 `name`）：

```bash
curl -s --noproxy '*' -X POST http://127.0.0.1:18099/mcp \
  -H "Content-Type: application/json" -H "Accept: application/json" \
  -H "MCP-Protocol-Version: 2026-07-28" -H "Mcp-Method: tools/call" \
  -H "Mcp-Name: db_info" \
  -d '{"jsonrpc":"2.0","id":"1","method":"tools/call","params":{"name":"db_info","arguments":{"db":"sqlite3"}}}'
```

Open WebUI 等只认 OpenAPI 的平台，可用社区 [mcpo](https://github.com/open-webui/mcpo) 代理把本服务转成 REST 再接入。

## 12. 占位：待验证客户端

以下客户端按 2026-09 现状**暂无经过验证的接入配置**，留位待补（接入条件都是"支持 stdio MCP server 配置"）：

| 客户端 | 状态 | 待验证点 |
| --- | --- | --- |
| **pi**（badlogic/pi coding agent） | 占位 | 其 MCP 配置文件位置与格式（`agent.json`?）；确认对 `input_required` 结果的处理 |
| **Reasonix** | 占位 | 是否支持本地 stdio MCP；配置入口 |
| **Grok build**（xAI） | 占位 | 官方 agent 的 MCP 支持面；若只有 remote MCP，可用 §11 的 `--http` + 反代暴露 |

> 有确切配置格式后，按 §1–§9 的模板补全本节即可——配置三要素恒为：`command`（start 脚本或 exe 绝对路径）、`args`（启动参数如 `--config`）、`cwd`（可选，建议 `D:/codes/zorm/zorm-mcp`）。

---

## 13. 客户端共性注意事项（FAQ）

- **`SQLITE_READONLY`，读正常写失败**：Windows 完整性级别问题（Claude Code 等以低完整性孵化 MCP 子进程），非 server bug。修复方法（icacls 降级目录与文件标签）见 `demo/README_demo.md` 第六节。**最省事**：像本指南一样用 start 脚本把数据库固定在仓库目录内（通常已是低完整性可写）。
- **AI 乱选工具 / 写错 `db` 参数**：报错自带可用后端清单和替代工具建议（自纠错设计）；服务器 `instructions` 已含完整决策表。仍不理想时，把 `db://guide` 资源或 `db-playbook` prompt 喂给会话。
- **`db_exec_sql` 看不到**：默认不注册，`--allow-sql` 才暴露——这是刻意设计（结构化写工具已覆盖绝大多数场景）。
- **客户端日志显示 `Process exited with code 3221225781`（0xC0000135 `STATUS_DLL_NOT_FOUND`）**：exe 同目录缺运行期 DLL——客户端（VS Code、从资源管理器启动的终端等）没有 MSYS2 的 PATH，`libc++.dll`/`libmariadb.dll` 等解析不到。重新构建对应目标，POST_BUILD 会按依赖闭包把 DLL 复制到 exe 目录（正常应看到 `zorm: bundled N resolved runtime DLL(s)`）；不要把 `D:/msys64/clang64/bin` 塞进客户端配置来绕过。
- **协议兼容性**：stdio 模式兼容 2024-11-05 → 2026-07-28 全系版本（legacy initialize + 新版 server/discover 双握手）；严格客户端（DSH）验证过 CacheableResult 字段。
- **要长期接生产库**：建议 `--config` 用绝对路径 + 专用的低权限数据库账号（只授业务库权限），不要复用管理员账号。
