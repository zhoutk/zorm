# zorm-mcp — 多后端数据库操作 MCP（正式版）

**一句话**：一个 MCP server，让 AI 用**同一套结构化语义**操作 sqlite / jsonfile / mysql / postgres / dm8 五种后端——不写一行 SQL 方言；写操作全部经过 MRTR 人机确认。

由 `demo/`（Zorm Analytics MCP 演示）验证过的架构升级而来：

| | demo（验证版） | zorm-mcp（本目录，正式版） |
| --- | --- | --- |
| 工具 | 6 个 | **13 个**：Idb 八接口全覆盖 + 表结构/索引/版本元数据 + 参数化只读 SQL + 事务 |
| 表结构获取 | 查表猜列（空表失效） | **catalog 元数据**（pragma table_info / information_schema / user_tab_columns / jsonfile columns 数组），空表也返回完整列定义 |
| AI 理解 | 基础描述 | 分层设计：instructions 决策表 + 工具路由提示 + 自纠错报错 + playbook prompt/资源 |
| 代码形态 | 单文件 1700 行 | 多文件模块化（app / guide / meta / data / sql / resources） |

## 构建

```bash
cmake --preset clang-rlz && cmake --build build --target zorm_mcp_server
# exe 与运行 DLL 自动落在 build/zorm-mcp/（或 build/bin/，随构建树）
```

零依赖开箱即用：不配置时内置 `sqlite3`（当前目录 `zorm-mcp.db`）与 `jsonfile`（`zorm-mcp.json`）两个后端。

## 接入 AI 客户端

**👉 全客户端配置手册见 [INTEGRATION.md](INTEGRATION.md)**——覆盖 Claude Code / Claude Desktop / DeepSeek Harness / VS Code & Copilot / Cursor / Windsurf / Cline & Roo Code / Codex CLI / Gemini CLI / MCP Inspector / 通用 HTTP，含占位待验证客户端清单。

快速接入（统一走 start 脚本，把数据库文件钉在本目录；**推荐带 `--config` 显式指定数据库**，见 [INTEGRATION.md §0.2](INTEGRATION.md) 的原因说明）：

```bash
# Claude Code（最简：内置 sqlite3 + jsonfile，默认库文件落在本目录）
claude mcp add zorm-mcp -- D:/codes/zorm/zorm-mcp/start.sh

# 推荐：--config 显式指定数据库（模板见 zorm-mcp.json，connString 写绝对路径）
claude mcp add zorm-mcp -- D:/codes/zorm/zorm-mcp/start.sh --config D:/codes/zorm/zorm-mcp/zorm-mcp.json
```

```jsonc
// 其他客户端通用形态（各客户端配置文件位置见 INTEGRATION.md）
{ "mcpServers": { "zorm-mcp": {
    "command": "D:/codes/zorm/zorm-mcp/start.sh",
    "args": ["--config", "D:/codes/zorm/zorm-mcp/zorm-mcp.json"]
} } }
```

## 工具一览（全部带 `db` 路由参数 = 后端注册名）

| 工具 | 写? | 对应 Idb | 说明 |
| --- | --- | --- | --- |
| `db_list_tables` {db} | | — | 用户表清单 |
| `db_describe_table` {db, table} | | querySql(内部) | **表字段元数据**：列名/类型/可空/默认值/主键，空表可用（catalog 直查，不靠查表猜列） |
| `db_indexes` {db, table} | | querySql(内部) | 索引清单（各方言 best-effort） |
| `db_info` {db} | | querySql(内部) | 后端类型 / 服务器版本 / 连通性 |
| `db_query` {db, table, …} | | select | 智能查询：fields / lks / ins / ors / group / sum / sort / page / size + 列条件（`{price:">,100"}` 比较前缀） |
| `db_count` {db, table, where?} | | select | 计数 |
| `db_query_sql` {db, sql, values?} | | querySql | 参数化**只读** SQL（SELECT/WITH/PRAGMA/SHOW/EXPLAIN；JOIN/方言函数/EXPLAIN 的出口） |
| `db_insert` {db, table, data} | ✅MRTR | create | 单行插入（缺 id 自动生成） |
| `db_insert_batch` {db, table, rows, constraint?} | ✅MRTR | insertBatch | 多行 upsert（冲突列默认 id） |
| `db_update` {db, table, data, where:{id}} | ✅MRTR | update | 单行更新 |
| `db_delete` {db, table, where} | ✅MRTR | remove/execSql | `{id}` 删一行；`{}` 清全表（确认文案带行数预估） |
| `db_trans` {db, sqls} | ✅MRTR | transGo | 原子事务：`{text|sql, values}` 或结构化 `{table, method, params, id}` |
| `db_exec_sql` {db, sql} | ✅MRTR | execSql | 任意写/DDL SQL，**`--allow-sql` 才注册** |

配套 MCP 能力：`db://schema`（资源：总览 / 每库 / 每表完整列元数据）、`db://guide` + `db-playbook` prompt（用法手册）、completion（db/table/fields 补全）。

## 让 AI 用对工具：四层理解设计

1. **推送层（主）**：`instructions` 决策表随连接注入模型上下文；每个工具 description 写明“何时用我 / 何时该用别的工具”，schema 属性带语义说明。
2. **自纠错层**：报错即教学——unknown db 附可用后端清单、不支持的条件给替代方案、只读 SQL 拒绝时指路写工具。
3. **拉取层（兜底）**：`db-playbook` prompt 与 `db://guide` 资源承载完整手册（不支持 instructions 的客户端用）。
4. **schema 即文档**：`db_describe_table` + resources 让 AI 不猜列名。

## MRTR 写确认

所有写工具首次调用**不执行**：返回 `resultType=input_required` + `requestState`，确认文案含 目标库/表/影响行数。客户端（AI 转述给用户）批准后，带原参数 + `requestState` + `inputResponses:{confirm:{approved:true}}` 重试即执行；`approved:false` 取消。对客户端回传形态做了宽泛容错（详见 demo-design.md §2.5）。

## 命令行参数

| 参数 | 说明 |
| --- | --- |
| `--config <file>` | 多后端配置（`{"databases": {名字: {type, options}}}`）；缺省内置 sqlite3 + jsonfile |
| `--http <port>` | Streamable HTTP 模式（`http://127.0.0.1:<port>/mcp`），默认 stdio |
| `--yes` | 关闭 MRTR 确认（自动化/测试专用） |
| `--allow-sql` | 暴露 `db_exec_sql`（默认隐藏） |
| `--child` | 内部参数：e2e 测试孵化用 |

## 测试

```bash
ctest -R mcp_test_zorm_mcp -V    # e2e：真子进程 stdio，覆盖 元数据/智能查询/只读SQL闸门/批量/事务/MRTR/补全/优雅关停
./run-test all                   # 核心库回归门（zorm 零改动硬约束）
```

## 常见问题

- **写入报 `SQLITE_READONLY` 但读正常**：Windows 完整性级别问题（Claude Code 以低完整性孵化 MCP 子进程），不是 server bug。修复见 `demo/README_demo.md` 第六节（icacls 降级目录与文件标签）。
- **`db` 参数写错**：报错会列出全部可用后端名，按提示改正（是 `sqlite3` 不是 `sqlite`）。
- **mysql/pg/dm8 连不上**：确认对应数据库服务器在运行、DLL 在 exe 同目录（构建时自动复制）。
