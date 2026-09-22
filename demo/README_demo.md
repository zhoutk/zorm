# Zorm Analytics MCP — 演示操作指南

**一句话**：让 AI 用自然语言同时操作多种异构数据库（sqlite / jsonfile 开箱即用，mysql / postgres / dm8 加配置即用），全程不懂任何 SQL 方言——这正是 zorm（统一 ORM 语义层）+ mcp-cpp（官方缺位的 C++ MCP SDK）的组合价值。

演示默认只用 **sqlite + jsonfile** 两个零依赖后端（远程数据库服务不需要）。

---

## 一、30 秒自演示（无需任何 AI 客户端）

```cmd
:: Windows（仓库根目录）
demo\run-demo.cmd
```
```bash
# Linux/macOS/MSYS2
./demo/run-demo.sh
```

脚本自动完成：建库建表（幂等）→ 启动真实子进程 MCP server → 用 mcp-cpp 自己的 client API 跑完整剧本：

1. `server/discover` —— 版本协商与能力宣告
2. `tools/list` —— 6 个数据库工具（无一行 SQL）
3. `db_list_tables` —— 表发现
4. `db_query` —— **智能查询**：`lks:{order_date:"2026"}` + `group:"region"` + `sum:"amount"`，按地区汇总 2026 订单
5. 跨库：同一语义查 **jsonfile** 的产品目录
6. `resources/read` —— server 自描述表结构
7. `db_insert` —— **MRTR 人机确认**：server 返回 `input_required`，暂停展示确认文案，"人类批准"后重试落盘
8. `db_count` —— 证明数据真的写进去了
9. stdin EOF —— 优雅关停

## 二、接入真实 AI 客户端（自然语言模式）

先构建并初始化（一次性）：

```cmd
cmake --preset clang-dbg && cmake --build build --target zorm_analytics_server
build\demo\zorm_analytics_server.exe --setup
```

### Claude Code

```bash
claude mcp add zorm-analytics -- D:/codes/zorm/build/demo/zorm_analytics_server.exe
```

然后在会话里直接说：

> 把 sqlite 里订单表的 2026 年订单按地区分组汇总金额，和 jsonfile 产品目录里单价超过 100 的产品做个对比，最后把汇总结果存到 jsonfile 的 analytics 表。

AI 会自动：发现表 → 读 schema → 智能查询跨库 → 发起写入 → 弹出 MRTR 确认 → 落盘。

### VS Code / Cursor（settings.json / mcp.json 片段）

```json
{
  "mcp": {
    "servers": {
      "zorm-analytics": {
        "command": "D:/codes/zorm/build/demo/zorm_analytics_server.exe"
      }
    }
  }
}
```

### MCP Inspector（看协议细节）

```bash
npx @modelcontextprotocol/inspector build/demo/zorm_analytics_server.exe
```

UI 里可以逐个调用工具、读 Resources、观察 `input_required` 确认流。

## 三、工具一览

| 工具 | 写? | 说明 |
| --- | --- | --- |
| `db_list_tables` `{db}` | | 列出某后端的所有表 |
| `db_query` `{db, table, fields?, lks?, ins?, ors?, group?, sum?, sort?, page?, size?}` | | 智能查询（zorm 保留字语义，方言层消化） |
| `db_count` `{db, table, where?}` | | 计数 |
| `db_insert` `{db, table, data}` | ✅MRTR | 插入 |
| `db_update` `{db, table, data, where}` | ✅MRTR | 更新 |
| `db_delete` `{db, table, where}` | ✅MRTR | 删除 |
| `db_exec_sql` `{db, sql}` | ✅MRTR | **默认不注册**，`--allow-sql` 才暴露 |

## 四、命令行参数

| 参数 | 说明 |
| --- | --- |
| `--setup` | 建库建表 + 样例数据（幂等），完成后退出 |
| `--demo` | 自演示剧本（自 spawn 真实子进程 + client API 驱动） |
| `--http <port>` | 额外开启 Streamable HTTP 端点（stdio 同时可用） |
| `--yes` | 关闭写确认（自动化/测试专用，**勿在演示中使用**——确认环节是亮点） |
| `--allow-sql` | 暴露参数化 SQL 工具 |
| `--config <file>` | 多后端配置（mysql/pg/dm8 接入点），默认内置 sqlite + jsonfile |
| `--child` | 内部参数：被 `--demo` 孵化时使用 |

## 五、常见问题

- **报找不到 DLL（LIBPQ.dll 等）**：用 `run-demo.cmd`/`run-demo.sh` 启动（已设好 `thirds/bin/win10` 路径），或把该目录加入 PATH。
- **演示数据想重置**：删除 `demo.db` 与 `analytics.json` 后重跑 `--setup`。
- **接入 mysql/postgres/dm8**：写一个 config JSON（参见 `demo-design.md` §2.6），用 `--config` 启动；这些后端需要对应数据库服务器在运行。
- **AI 不会写确认弹窗**：Claude Code 等客户端对 elicitation 的支持程度不同；若客户端不支持，AI 会看到 `input_required` 的确认请求并转述给用户，用户回复后 AI 带 `inputResponses` 重试——这正是 MRTR 的设计意图。
