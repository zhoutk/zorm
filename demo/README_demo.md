# Zorm Analytics MCP — 演示操作指南

> **正式版已落地**：本演示验证的架构已产品化为仓库根目录的 [`zorm-mcp/`](../zorm-mcp/README.md)（13 工具全 Idb 覆盖 + catalog 级表结构元数据 + 事务/只读 SQL 闸门）。接入配置见 [`zorm-mcp/INTEGRATION.md`](../zorm-mcp/INTEGRATION.md)。本文档保留为演示剧本与 Windows 完整性级别 FAQ 的权威说明。

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

**演示模式**：

| 命令 | 模式 |
|------|------|
| `run-demo.cmd` / `./demo/run-demo.sh` | **自动模式**（默认，`--auto true`），连贯播放完整剧本 |
| `run-demo.cmd --auto false` / `./demo/run-demo.sh --auto false` | **分步模式**：每步结束后暂停，提示「验证方法」（如用 `sqlite3 demo.db ".tables"` 交叉验证），按回车继续，输入 `q` 提前退出 |

**两个演示数据库文件**（演示开场会打印绝对路径）：

| 数据库 | 文件 | 角色 | 说明 |
|--------|------|------|------|
| sqlite | `demo.db`（项目根目录） | **测试数据** | orders(500)/users(50)/products(20) |
| jsonfile | `demo.json`（项目根目录） | **输出库** | 初始为空，演示过程写入汇总结果 |

**数据流设计**：测试数据放 sqlite，jsonfile 是输出库。查询统计基于 sqlite，结果写入 jsonfile——json 内容少、方便查看。

脚本自动完成：建库建表（幂等）→ 启动真实子进程 MCP server → 用 mcp-cpp 自己的 client API 跑完整剧本：

1. `server/discover` —— 版本协商与能力宣告
2. `tools/list` —— 6 个数据库工具（无一行 SQL）
3. `db_list_tables` —— sqlite 测试库的表发现
4. `db_query` —— **智能查询**：`lks:{order_date:"2026"}` + `group:"region"` + `sum:"amount"`，按地区汇总 2026 订单（数据在 sqlite）
5. `db_insert` —— 汇总结果**写入 jsonfile.region_sales**（输出库落盘，MRTR 确认）
6. `resources/read` —— server 自描述表结构
7. `db_query` 完整对比 —— 2026 订单汇总 vs 单价>100 产品（`price:">,100"` 比较操作符）
8. `db_insert` —— 对比结论**写入 jsonfile.analytics**（MRTR 确认）
9. `db_count` —— 验证两个输出表都写入了
10. `db_delete` —— 收尾：清空 analytics 统计空表，保留 region_sales 汇总
11. stdin EOF —— 优雅关停

**最终 jsonfile**：`region_sales`（5 行地区汇总）+ `analytics`（空，统计空表容器）。

> **终端编码**：演示输出为 UTF-8。cmd / PowerShell 下程序会自动切换控制台代码页（`SetConsoleOutputCP(CP_UTF8)`），无需手动 `chcp 65001`；MSYS2 mintty 默认 UTF-8 可直接显示。若在自定义终端仍遇乱码，请确认终端字符集为 UTF-8。

## 二、接入真实 AI 客户端（自然语言模式）

先构建并初始化（一次性）：

```cmd
cmake --preset clang-rlz && cmake --build build --target zorm_analytics_server
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

### DeepSeek Harness（dsh）

DeepSeek Harness 使用 MCP SDK 2.0（协议 2026-07-28），是**严格客户端**：`tools/list` 等结果缺 `ttlMs`/`cacheScope` 会直接拒绝（表现为 `Invalid result for tools/list: ttlMs expected number` → `server is disconnected`）。因此**必须用最新构建的 server**（mcp-cpp 已默认注入这两个字段），Claude Code 等宽容客户端能掩盖旧构建的问题，DSH 不能。

**接入方法**（profile patch 层，不新建文件）：

1. 编辑 `~/.dsh/profiles/web/cordis.patch.yml`（Windows: `C:\Users\<user>\.dsh\profiles\web\cordis.patch.yml`），在数组末尾**追加**：

```yaml
# Zorm Analytics MCP (C++ stdio server, sqlite + jsonfile backends).
# Tools surface as mcp__zorm-analytics__<tool>.
- insert:
    - id: mcp-zorm-analytics
      name: '@deepseek-ai/dsh-mcp-client'
      config:
        serverName: zorm-analytics
        transport: stdio
        command: D:/codes/zorm/build/demo/zorm_analytics_server.exe
        cwd: D:/codes/zorm
```

   `cwd` 指到仓库根目录，server 打开的就是根目录下的 `demo.db` / `demo.json`。

2. 保存即生效——profile 默认 `patchReload: "live"`（`package.json` 里 `dsh.profile.patchReload`），热加载并重连，无需重启 dsh。日志出现 `mcp-client(zorm-analytics)` 注册即成功；若看到 `server is disconnected`，先确认 exe 是最新构建再排查。

3. 会话里直接问（与 Claude Code 相同的自然语言用法）：

> 根目录下的 demo.db 中有哪些表？

> 把 sqlite 里订单表的 2026 年订单按地区分组汇总金额，结果存到 jsonfile 的 analytics 表。

**验证要点**：`tools/list` 应列出 6 个工具（`mcp__zorm-analytics__db_list_tables` 等）；写入工具触发 MRTR `input_required` 确认。DSH 对结果信封校验严格，通过即说明 2026-07-28 协议兼容性完好。

### curl 冒烟（HTTP 模式）

```bash
build\demo\zorm_analytics_server.exe --http 18099 --yes &
curl -s --noproxy '*' -X POST http://127.0.0.1:18099/mcp   -H "Content-Type: application/json" -H "Accept: application/json"   -H "MCP-Protocol-Version: 2026-07-28" -H "Mcp-Method: server/discover"   -d '{"jsonrpc":"2.0","id":"1","method":"server/discover","params":{"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28","io.modelcontextprotocol/clientCapabilities":{}}}}'
```

### MCP Inspector（看协议细节；HTTP 模式下选 SSE/Streamable HTTP 传输并填 URL）

```bash
npx @modelcontextprotocol/inspector build/demo/zorm_analytics_server.exe
```

UI 里可以逐个调用工具、读 Resources、观察 `input_required` 确认流。

## 三、工具一览

所有工具的 `db` 路由参数取**后端注册名**：默认内置 `sqlite3`（demo.db）与 `jsonfile`（demo.json）。注意是 `sqlite3` 不是 `sqlite`——与 zorm 的后端类型名保持一致，AI 客户端容易想当然写成 `sqlite`；写错会收到列出全部可用后端名的报错，按提示改正即可。

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
| `--http <port>` | **纯 HTTP 模式**（不跑 stdio）：Streamable HTTP 单端点 `http://127.0.0.1:<port>/mcp`，浏览器 Inspector / curl / 远程 AI 客户端接入用 |
| `--yes` | 关闭写确认（自动化/测试专用，**勿在演示中使用**——确认环节是亮点） |
| `--allow-sql` | 暴露参数化 SQL 工具 |
| `--config <file>` | 多后端配置（mysql/pg/dm8 接入点），默认内置 sqlite + jsonfile |
| `--child` | 内部参数：被 `--demo` 孵化时使用 |

## 五、常见问题

- **报找不到 DLL（LIBPQ.dll 等）**：构建时已自动把运行 DLL 复制到 exe 同目录（`build/demo/`），正常无感；若手动搬移 exe，请连 DLL 一起带或用 `run-demo.cmd`/`run-demo.sh` 启动。
- **AI 客户端显示连接失败**：确认用的是最新构建——服务器现在支持 legacy `initialize` 握手（Claude Code/VS Code 的接入方式）与无 `_meta` 请求，协议版本 2024-11-05 全系支持。改代码后需重新构建并重启 AI 客户端会话。
- **严格客户端（DeepSeek Harness / MCP SDK 2.0+）报 `Invalid result for tools/list: ttlMs expected number`**：2026-07-28 规范要求 `*/list` 与 `resources/read` 结果必带 `ttlMs`/`cacheScope`（CacheableResult），mcp-cpp 现已默认注入（`ttlMs 0`/`public`，`listCache(-1)` 显式退出）。旧构建缺这两个字段，宽容客户端（Claude Code）不校验所以无感——重新构建 server 即可。
- **演示数据想重置**：删除 `demo.db` 与 `demo.json` 后重跑 `--setup`。
- **接入 mysql/postgres/dm8**：写一个 config JSON（参见 `demo-design.md` §2.6），用 `--config` 启动；这些后端需要对应数据库服务器在运行。
- **AI 不会写确认弹窗**：Claude Code 等客户端对 elicitation 的支持程度不同；若客户端不支持，AI 会看到 `input_required` 的确认请求并转述给用户，用户回复后 AI 带 `inputResponses` 重试——这正是 MRTR 的设计意图。服务器对客户端回传确认的多种形态做了容错（`confirm`/`content`/`value`/`params` 嵌套或顶层 `approved`，及 `action: accept/decline`），详见 `demo-design.md` §MRTR。
- **`SQLITE_READONLY`，写入失败，但文件可写、磁盘无只读、属性只有 `A`**：这是 Windows 的**完整性级别（Integrity Level）**问题，不是文件只读，也不是服务器 bug。Claude Code 在 Windows 上以**低完整性（Low integrity）**运行 MCP 服务器等原生子进程；低完整性进程**能读**普通（中完整性）文件、**不能写**（无"写升"）。于是查询/读表一切正常，只有写操作报 `SQLITE_READONLY`。**最容易触发的情形**：在 Claude Code 会话里用 `mkdir`/`! mkdir` 在 Windows 下新建一个目录（如 `D:\tmp`）并把 `demo.db` 放进去——新目录默认中完整性，低完整性服务器写不进去。修复：把库文件和所在目录的完整性级别降为 Low（`icacls /setintegritylevel Low`，见下一节「六」），或把库放到能被低完整性进程写入的位置。

---

## 六、Windows 完整性级别（Integrity Level）与 icacls

### 6.1 为什么会遇到：Claude Code 的低完整性沙箱

从 Windows Vista 起，系统有**强制完整性控制（Mandatory Integrity Control）**：每个进程与文件/目录都带一个完整性级别标签，级别从低到高为 **Untrusted → Low → Medium → High → System**。规则是进程可以**读同级或更低**、**写同级**，但**不能写比它更高完整性级别的对象**（无"写升"，Write-Up 被禁）。默认新建的目录与文件是**中完整性（Medium）**。

Claude Code 出于进程沙箱安全，在 Windows 上以**低完整性（Low）**运行其原生子进程——MCP 服务器正是其中之一。因此：

| 操作 | 对象 | 结果 |
| --- | --- | --- |
| 读 | 中完整性数据库文件 | ✅ 允许（查询/`db_list_tables` 正常） |
| 写 | 中完整性数据库文件 | ❌ 拒绝（`ACCESS_DENIED`）→ SQLite 退回只读 → `SQLITE_READONLY` |
| 写 | 低完整性数据库文件 | ✅ 允许 |

SQLite 在写打开失败时**静默降级为只读打开**（`sqlite3_open_v2` 返回成功但连接是只读的），直到 `sqlite3_step` 写那条才爆出 `SQLITE_READONLY`——所以现象永远是"读得好好的，一写就报只读"，极易误判为文件权限/磁盘只读/服务器配置。

**判定方法**：在 PowerShell / cmd 里执行 `icacls "<路径>"`。文件若含 `Mandatory Label\...:(NW)` 这一行，说明被显式标记了完整性级别；若无此行的常规文件默认中完整性。也可用进程工具确认：Claude Code 孵化的 server 进程完整性级别为 Low（0x1000）。

### 6.2 icacls 功能简介

`icacls`（Integrated Cacls）是 Windows 自带的 ACL 查看/修改工具，既管 DACL（谁对谁有什么权限），也管完整性标签（MIC）。基本形态：

```
icacls <path> [/grant <user>:(权限)] [/remove <user>] [/setintegritylevel <级别>] [/T] [/inheritance:r] ...
```

常用子命令：

| 子命令 | 作用 |
| --- | --- |
| `icacls <path>` | 查看路径的 ACL（含继承标记与 `Mandatory Label\` 完整性行） |
| `icacls <path> /grant <user>:(OI)(CI)F` | 授予用户权限。`F`=完全控制、`M`=修改、`RX`=读执行、`W`=写。`(OI)`=对象继承、`(CI)`=容器继承（目录专用，让子文件/子目录继承） |
| `icacls <path> /remove <user>` | 移除该用户的权限 |
| `icacls <path> /setintegritylevel [(CI)(OI)] Low` | **设置完整性级别**（`Low`/`Medium`/`High`/`System`），不触碰任何人的 DACL 权限 |
| `icacls <path> /T` | 递归应用到所有子文件/子目录 |
| `icacls <path> /inheritance:r` | 移除继承，仅保留显式 ACE |
| `icacls <path> /reset` | 重置为继承父级的 ACL |

### 6.3 用 icacls 修复"低完整性进程写不进库"

目标：把 `demo.db` 及其所在目录的完整性级别设为 Low，使低完整性的 MCP 服务器可以写入。在 **PowerShell / cmd** 里执行：

```powershell
icacls "D:\tmp" /setintegritylevel Low
icacls "D:\tmp\demo.db" /setintegritylevel Low
```

**两条都要**，原因：
- **目录**设 Low —— SQLite 用 DELETE 日志模式写库时，要在**同目录**创建 `demo.db-journal` 日志文件，目录若是中完整性，低完整性进程连日志都建不了。
- **文件**设 Low —— 已存在的文件**不会**自动继承目录新设的标签，必须对文件本身单独 `setintegritylevel`。（若目录已设为 Low，之后再新建/放进去的文件会自动继承 Low，可只设目录一次。）

验证是否生效：`icacls "D:\tmp\demo.db"` 应出现 `Mandatory Label\Low Mandatory Level:(NW)` 一行，然后用客户端重跑 `db_insert`。

> **MSYS2/Git Bash 注意事项**：MSYS2 会把以 `/` 开头的参数误当作路径转换（如 `/setintegritylevel` 被改成 `D:/msys64/setintegritylevel`）。在 Git Bash 里请加 `MSYS2_ARG_CONV_EXCL='*'` 前缀，或干脆用 PowerShell/cmd 执行：
>
> ```bash
> MSYS2_ARG_CONV_EXCL='*' icacls 'D:\tmp' /setintegritylevel Low
> ```

**可逆性**：`/setintegritylevel` 只改完整性标签，不删除任何人的读写权限、不动数据；把 `Low` 改回 `Medium` 即完全恢复。它不属于"修改 ACL 授权"，但确实是安全级别的放宽——只应在需要让沙箱进程写入的目录上使用。

### 6.4 其他可选方案

- **换一个低完整性可写的目录**：仓库 `build/` 等由工具链生成的目录树往往已被标为 Low，把库放那里即可无感写入（本演示在仓库根目录的 `demo.db` 即属此列）。
- **让 Claude Code 的 MCP 服务器以正常完整性运行**（不走低完整性沙箱）：属进程启动层面配置，视客户端版本支持而定；若可关闭 Windows 子进程沙箱，则无需改任何文件 ACL。服务器对客户端回传确认的多种形态做了容错（`confirm`/`content`/`value`/`params` 嵌套或顶层 `approved`，及 `action: accept/decline`），详见 `demo-design.md` §MRTR。
- **`SQLITE_READONLY`，写入失败，但文件可写、磁盘无只读、属性只有 `A`**：这是 Windows 的**完整性级别（Integrity Level）**问题，不是文件只读，也不是服务器 bug。Claude Code 在 Windows 上以**低完整性（Low integrity）**运行 MCP 服务器等原生子进程；低完整性进程**能读**普通（中完整性）文件、**不能写**（无"写升"）。于是查询/读表一切正常，只有写操作报 `SQLITE_READONLY`。**最容易触发的情形**：在 Claude Code 会话里用 `mkdir`/`! mkdir` 在 Windows 下新建一个目录（如 `D:\tmp`）并把 `demo.db` 放进去——新目录默认中完整性，低完整性服务器写不进去。修复：把库文件和所在目录的完整性级别降为 Low（`icacls /setintegritylevel Low`，见下一节「六」），或把库放到能被低完整性进程写入的位置。
