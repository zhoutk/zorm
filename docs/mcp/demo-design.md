# zorm × mcp-cpp 联合演示项目设计方案（v2 执行版）

> 目标：做一个让人"眼前一亮"的演示，同时体现 zorm（统一多后端 ORM + 智能查询）与 mcp-cpp（独立 C++ MCP SDK）两个项目的核心优势。
> 日期：2026-09-22 · 分支 `mcp`
> v1→v2 修订：①任务与迁移计划/review 合流（demo 即迁移计划阶段 2/3/5 的载体）；②新增 `--scripted-demo` 自演示模式（免 AI 客户端即可看全部亮点）；③文件级实现清单 + 逐步测试方法 + 最终演示操作脚本；④明确推迟项清单（B1 性能等与演示无关项）。
> 现状基础：`thirds/mcp-cpp`（M0–M5 完成 + 评审 P0 已修 `9b854b3`）+ `examples/zorm_server.cpp`（单库四工具）。

---

## 〇、任务合流：三份文档一条线（v2 核心修订）

三份文档的任务高度重叠，**不分轨推进**：

| 来源 | 任务 | 去向 |
| --- | --- | --- |
| 迁移计划 阶段 2 待办 | 写操作 MRTR 确认闸门 | → demo 步骤 2 |
| 迁移计划 阶段 3 | Resources `db://schema` + completion + 分页 | → demo 步骤 2 |
| 迁移计划 阶段 5 | zorm 集成 e2e + 文档 | → demo 步骤 3 |
| review §六.1 | zorm 集成旗舰化（池化/确认/Resources/补全） | → demo 步骤 2（池化推迟） |
| review P2/P3 | B1 零拷贝、schema 预编译、cancellations 优化 | → **推迟**（见 §七），与演示无关 |
| review §六.3/4 | conformance、Tasks/OAuth | → **推迟** |

**执行顺序**：demo 步骤 1→4（§四），完成后迁移计划阶段 2/3/5 同步勾销。

---

## 一、Demo 概念："多库智能数据分析助手"（Zorm Analytics MCP）

一句话：**让 AI 用自然语言，同时查询和操作多种异构数据库，全程不懂 SQL。**

### 1.1 亮点脚本（演示话术）

```
用户（Claude/VS Code/Cursor）：
"把 sqlite 里订单表的 2026 年订单，按地区分组汇总金额，
 再看看 jsonfile 里存的产品目录里单价超过 100 的有哪些，
 最后把汇总结果存到 jsonfile 的 analytics 表里做留存。"

AI 行为（一次会话内，透明跨库）：
1. [sqlite]   db_list_tables {db:"sqlite"}           → 发现 orders 表
2. [sqlite]   db_query {db:"sqlite", table:"orders", lks:{order_date:"2026"},
                        group:["region"], sum:["amount"]}   → 按地区汇总
3. [jsonfile] db_query {db:"jsonfile", table:"products", gtn:{price:100}} → 过滤
4. [jsonfile] db_insert {db:"jsonfile", table:"analytics", data:{...}}
              → MRTR 确认 → 用户批准 → 落盘留存
```

"眼前一亮"点：
- AI 不懂任何 SQL 方言，全程同一套 `db_query` 语义（zorm 方言层消化）；
- 一个 MCP server 内多后端路由（多 `DbBase` 实例）；
- 写操作有 MRTR 人机确认（human-in-the-loop 最佳实践）；
- sqlite + jsonfile 默认零部署，开箱即跑；全程 C++17 无运行时。

### 1.2 与"裸 SQL 直通"的差异（保留 v1，叙事核心）

| 维度 | 裸 SQL MCP（市面） | 本 demo（zorm） |
| --- | --- | --- |
| 方言 | 每库一套 SQL 语法 | 统一 `db_query` 语义，方言层消化 |
| 安全性 | 裸 SQL 注入面大 | 结构化智能查询（lks/ins/gtn…）+ 写确认 |
| 表发现 | 客户端需知道表名 | `db_list_tables` + Resources 自描述 + 补全 |
| 跨库 | 一个 server 一个库 | 一个 server 多后端按 `db` 路由 |
| 部署 | 需数据库服务器 | sqlite + jsonfile 零依赖开箱即跑 |

---

## 二、技术设计

### 2.1 架构

```
┌───────────────────────────────────────────────────────────┐
│  AI 客户端（Claude / VS Code / Cursor / Inspector）         │
└───────────────┬───────────────────────────────────────────┘
                │ stdio（主）/ Streamable HTTP（可选 --http）
┌───────────────▼───────────────────────────────────────────┐
│  zorm_analytics_server（demo 可执行，链接 mcp + zormlib）   │
│                                                           │
│  ┌────────────────────────────────────────────────────┐   │
│  │ mcp-cpp Server（协议层，已就绪）                      │   │
│  │  tools + resources + completion + MRTR + 传输        │   │
│  └────────────────┬───────────────────────────────────┘   │
│  ┌────────────────▼───────────────────────────────────┐   │
│  │ zorm 适配层（demo 新增，examples/ 内）                │   │
│  │  · 多后端路由：db 参数 → DbBase 实例表               │   │
│  │  · 元数据查询：各库表清单/列结构（内部参数化 querySql）│   │
│  │  · 智能查询透传：lks/ins/ors/group/sum/sort/分页      │   │
│  │  · 写确认状态机：pending 操作表（requestState→操作）  │   │
│  └────────────────┬───────────────────────────────────┘   │
│  ┌────────────────▼───────────────────────────────────┐   │
│  │ zorm 核心（零改动，Idb.h 冻结）                       │   │
│  └────────────────────────────────────────────────────┘   │
└───────────────────────────────────────────────────────────┘
```

### 2.2 工具集

| 工具 | 参数 | 写? | 说明 |
| --- | --- | --- | --- |
| `db_list_tables` | `{db}` | 否 | 各库元数据（sqlite_master / information_schema / jsonfile 顶层键） |
| `db_query` | `{db, table, fields?, lks?, ins?, ors?, gtn?/gte?/ltn?/lte?, group?, sum?, count?, sort?, page?, size?}` | 否 | **智能查询全参数**（zorm 特色）；数字比较保留字按 zorm 实际支持的集合裁剪 |
| `db_count` | `{db, table, where?}` | 否 | 计数 |
| `db_insert` | `{db, table, data}` | ✅MRTR | 插入（内部走 insertBatch 单元素） |
| `db_update` | `{db, table, data, where}` | ✅MRTR | 更新 |
| `db_delete` | `{db, table, where}` | ✅MRTR | 删除（高危，确认文案带行数预估） |
| `db_exec_sql` | `{db, sql, params?}` | ✅MRTR | **默认不注册**，`--allow-sql` 才暴露 |

> 注意：数字比较保留字（gtn/gte/ltn/lte 等）以 zorm `QUERY_EXTRA_KEYS` 实际集合为准，设计文档不虚构；实现时从 `SqlBackendBase` 核对。

### 2.3 Resources（表结构自描述）

- `db://schema` → 所有后端的库清单与表清单总览；
- `db://schema/{db}/{table}` → 某表列/类型/主键（setup 已知表集合，启动时注册）；
- 写操作落盘后调用 `notifyResourceUpdated`（mcp-cpp 已支持）→ 配合订阅演示实时通知。
- v1 简化决策：**不做动态 resource 注册**——demo 的表集合在 setup 时已知，启动时静态注册全部；新增表走 `--setup` 幂等重建 + 重启。

### 2.4 Completion

- `completion/complete`：对 `table` 参数补全表名、对 `fields` 补全列名（handler 内部查元数据缓存）。

### 2.5 写操作 MRTR 确认闸门（设计定案）

```
首次 tools/call(db_insert)：
  handler → 组装 pending 操作（db/table/data，存 requestState→操作 map）
          → 返回 resultType:"input_required"
            { requestState:"st-1",
              inputRequests:{ confirm:{ method:"elicitation/create",
                                        params:{ message:"将向 sqlite.orders 插入 1 行，确认?" } } } }
重试 tools/call(db_insert, inputResponses:{confirm:{...approved:true}})：
  按 requestState 取回 pending 操作 → 执行 → 清除 → 返回 complete
inputResponses.approved != true → 返回 complete + content 说明"已取消"（不执行）
```

- 单线程 dispatch 下 pending map 无锁；每 server 实例一份；
- 确认文案必须含 目标库/表/影响行数预估（count 先行）；
- `--yes` 参数可关闭确认（仅供自动化测试与 scripted demo 内部使用）。

### 2.6 配置与数据源

```jsonc
// zorm_analytics.json（demo 默认，开箱即跑只含前两个）
{
  "databases": {
    "sqlite":   { "type": "sqlite3",  "options": { "connString": "demo.db" } },
    "jsonfile": { "type": "jsonfile", "options": { "connString": "demo.json" } },
    "mysql":    { "type": "mysql",    "options": { "..." } },   // 可选
    "postgres": { "type": "postgres", "options": { "..." } },   // 可选
    "dm8":      { "type": "dm8",      "options": { "..." } }    // 可选
  }
}
```

- 无配置文件时使用内置默认（sqlite + jsonfile，当前目录）；
- **首次运行自动建库建表 + 样例数据**（幂等：先探测表是否存在），"三秒看到效果"；
- 样例数据量：orders ~500 行（2025/2026 两年 × 5 地区）、users ~50、products ~20——让 group/sum 有真实感。

### 2.7 `--scripted-demo` 自演示模式（v2 新增，演示可用性的关键）

- server 内置一段用 **mcp-cpp client API** 驱动自身的剧本（`--demo` 时：自 spawn 子进程跑真 stdio 传输——复用 bench_e2e 的技法，顺带证明"自家 client 驱动自家 server"）；
- 剧本 = §1.1 亮点脚本的程序化版本：每步打印 工具名/参数/结果摘要，ANSI 彩色，最后打印"注册到 AI 客户端"的现成命令；
- 写确认演示环节由剧本自动"扮演用户"批准（终端暂停 2 秒展示确认文案）；
- **全程无需注册任何 AI 客户端**，一条命令 30 秒看完全部亮点。

---

## 三、详细实现计划（4 步，~4~6 天）

> **执行状态（2026-09-22）：四步全部完成。** M1 `0d02c60`（多后端路由 + 智能查询）、M2 `9b941e7`+`978cb8e`（MRTR/completion + e2e 5 用例，修复递归 spawn 卡死）、M3 `6daab23`（自演示剧本 + run-demo 脚本 + README_demo）、M4 `e7fe4b7`（--http 纯 HTTP 模式）。后续补充：db_query 的 size/page schema 边界 + 负路径 e2e 断言。回归：mcp 11/11 + zorm local/utils/pool 全绿。

### 步骤 1：demo 核心闭环（~1.5 天）

| # | 任务 | 产出 |
| --- | --- | --- |
| 1.1 | `examples/demo_data.h/.cpp`：样例数据生成（幂等建表：探测→create→批量 insert） | sqlite + jsonfile 各建 orders/users/products |
| 1.2 | `examples/zorm_analytics_server.cpp`：配置解析（内置默认 + `--config`）、`std::map<std::string, std::unique_ptr<ZORM::Idb>>` 多后端路由 | server 骨架 |
| 1.3 | `db_list_tables`：适配层元数据查询（sqlite: `querySql("select name from sqlite_master where type='table'")`；jsonfile: 读存储文件顶层键；其余库 information_schema，仅配置存在时编译路径） | 表发现 |
| 1.4 | `db_query`/`db_count`：智能查询全参数透传 zorm（fields/lks/ins/ors/group/sum/count/sort/page/size） | 查询能力 |
| 1.5 | 工具 inputSchema 全量声明（mcp-cpp schema 校验自动生效） | 参数防护 |
| 1.6 | 注册进根 CMakeLists（TARGET zormlib 守卫内） | `zorm_analytics_server` 目标 |

**步骤 1 验收门**：手跑 §五.T1/T2 全绿 + `./run-test all` 回归门全绿。

### 步骤 2：MRTR 写确认 + Resources + completion（~1.5~2 天）

| # | 任务 | 产出 |
| --- | --- | --- |
| 2.1 | 写确认状态机：pending map + `input_required` 组装 + inputResponses 处理（§2.5 流程） | `db_insert/db_update/db_delete` 安全闸门 |
| 2.2 | Resources：`db://schema` 总览 + `db://schema/{db}/{table}` 逐表注册（setup 表集合已知）；元数据缓存（表→列/类型） | 表结构自描述 |
| 2.3 | Completion：`b.completions(handler)` 对 table/fields 补全（读元数据缓存） | 参数补全 |
| 2.4 | 写后 `notifyListChanged/notifyResourceUpdated` 接线 | 订阅演示素材 |
| 2.5 | `db_exec_sql`：`--allow-sql` 白名单开关（默认不注册） | 高级能力（默认关） |

**步骤 2 验收门**：手跑 §五.T3/T4 全绿。

### 步骤 3：自演示 + 测试 + 文档（~1 天）

| # | 任务 | 产出 |
| --- | --- | --- |
| 3.1 | `--scripted-demo`：自 spawn + client API 驱动 + ANSI 彩色剧本（§2.7） | 一条命令演示 |
| 3.2 | `tests/test_zorm_analytics.cpp`：自 spawn e2e（复用 test_stdio_e2e 技法），用例见 §五 | 回归保护 |
| 3.3 | `examples/README_demo.md`：话术脚本 + 三客户端注册片段（Claude Code/VS Code/Inspector） | 文档 |
| 3.4 | CTest 注册 `mcp_test_zorm_analytics`（仅 TARGET zormlib 存在时） | CI 门 |

**步骤 3 验收门**：`ctest -R zorm_analytics` 全绿；全新目录克隆构建 → `--demo` 一次成功。

### 步骤 4（可选加分）：HTTP 模式 + 五库形态（~0.5 天）

| # | 任务 | 产出 |
| --- | --- | --- |
| 4.1 | `--http <port>`：`HttpServerTransport` 接线（复用同一 server 实例） | 浏览器 Inspector 远程演示 |
| 4.2 | mysql/postgres/dm8 配置示例 + `--config` 五库全开文档 | 五后端形态 |

**步骤 4 验收门**：`--http` 起 server，Inspector HTTP 连接完成一次 tools/call。

### 明确不做（推迟清单，demo 后另立计划）

- B1 零拷贝解析 / envelope 预扫描（review P2——演示延迟为毫秒级，无感知）
- schema 预编译缓存、cancellations 无序 map（review P3 微优化）
- DbPool 每请求租用（单线程 dispatch 下无收益，多 worker 时再做）
- conformance 对齐、Tasks 扩展、OAuth 2.1（各自独立立项）

---

## 四、测试计划（每步的方法与命令）

### T1. 单元/组件级（随步骤 1-2 开发随时跑）

| 用例 | 方法 |
| --- | --- |
| 建库幂等 | 连跑两次 `--setup`，第二次无副作用（行数不变） |
| 多后端路由 | `db_query{db:"sqlite"}` / `{db:"jsonfile"}` 各返回各自数据 |
| 智能查询保留字 | lks/ins/ors/group+sum/sort/page+size 逐个断言（对照 zorm 契约测试已知行为） |
| 未知 db/table | 返回 -32602（InvalidParams） |
| 非法参数 | 越界 size、缺 tablename → mcp-cpp schema 校验拦截（-32602） |

命令：`cmake --build build --target zorm_analytics_server && ctest -R zorm_analytics`（步骤 3 后纳入 CTest）。

### T2. stdio e2e（test_zorm_analytics.cpp，自 spawn 真子进程）

1. spawn 子进程（`--yes` 关确认用于常规路径）
2. discover → tools/list 断言 6 工具与 schema
3. resources/list 断言 db://schema 集合；resources/read 断言列结构
4. db_list_tables / db_query（含 group+sum 跨库）/ db_count 逐个断言结果
5. MRTR 流：不带确认调用 db_insert → 断言 `input_required` + requestState；带 `inputResponses.approved:true` 重试 → 断言 complete 且数据落盘；带 approved:false → 断言未执行
6. completion/complete：table 前缀补全断言
7. EOF 优雅退出

### T3. 手动冒烟（每步完成后）

| 检查 | 命令/操作 |
| --- | --- |
| 零依赖启动 | 全新目录 `--setup` + `--demo`（无任何数据库服务器） |
| Inspector stdio | `npx @modelcontextprotocol/inspector build/bin/zorm_analytics_server.exe` → UI 手动调工具 |
| 回归门 | `./run-test all` 全绿（核心库零改动硬约束） |

### T4. 真实 AI 客户端闭环（步骤 3 后，最终验收）

1. Claude Code 注册：`claude mcp add zorm-analytics -- build/bin/zorm_analytics_server.exe`
2. 输入 §1.1 话术，确认 AI 自动完成 跨库查询 → 汇总 → MRTR 确认弹窗 → 落盘
3. VS Code / Cursor 用 README_demo 片段重复一次
4. DeepSeek Harness 用 README_demo 的 cordis.patch.yml 片段重复一次（严格客户端，可验证 2026-07-28 CacheableResult 兼容性）
5. （步骤 4 后）`--http` + 浏览器 Inspector 走一遍

---

## 五、最终演示操作脚本（给观众/用户的"简单操作"路径）

```bash
# ① 构建（一次性）
cmake --preset clang-dbg && cmake --build build --target zorm_analytics_server

# ② 30 秒自演示（无需任何 AI 客户端）——自动建库 + 跨库查询 + 写确认全流程
build/bin/zorm_analytics_server.exe --demo

# ③ 接入真实 AI（README_demo 有各客户端现成片段）
claude mcp add zorm-analytics -- build/bin/zorm_analytics_server.exe
#    然后在 Claude Code 里直接说：
#    "把 sqlite 订单按 2026 年和地区分组汇总金额，和 jsonfile 的产品目录对比，结果存到 jsonfile"
```

观众视角的三层递进：`--demo` 看结果（30 秒）→ Inspector 看协议（交互）→ Claude Code 玩自然语言（惊艳）。

---

## 六、风险与对策

| 风险 | 对策 |
| --- | --- |
| 各库元数据 SQL 方言差异 | 只在适配层内部用参数化 querySql；sqlite/jsonfile 先行（零依赖），其余库有服务器才走 |
| jsonfile"表清单"无现成接口 | 从存储文件顶层键读取（jsonfile 后端单文件多表结构）；必要时给 JsonFileDb 加只读辅助（不改 Idb 签名） |
| MRTR 与真实 AI 客户端的兼容性 | elicitation/create 是规范标准方法；Claude Code 当前对 elicitation 支持程度需在 T4 实测，不支持时降级为"确认工具"（独立 confirm 工具 + requestState），文档如实记录 |
| 数字比较保留字与 zorm 实际集合不符 | 实现前核对 `QUERY_EXTRA_KEYS`，schema 按实际集合生成 |
| 演示当日环境 | `--demo` 不依赖网络与 AI 客户端；准备预构建 exe 兜底 |

---

## 七、验收标准（总体）

1. `zorm_analytics_server` 无数据库服务器即可跑（sqlite + jsonfile），一条命令自演示；
2. e2e 测试（真子进程 stdio）覆盖 跨库查询/写确认/Resources/补全，纳入 CTest；
3. 真实 AI 客户端完成"自然语言 → 跨库分析 → MRTR 确认 → 落盘"闭环；
4. 核心库零改动：`Idb.h` 冻结，`./run-test all` 全绿；
5. 迁移计划阶段 2/3/5 随本 demo 同步勾销。

---

*v1（2026-09-22）概念与调研仍有效；v2 为执行版：任务合流编排、--scripted-demo 自演示、文件级实现清单、T1-T4 测试矩阵、三层递进演示脚本。全部能力基于已验证代码（mcp-cpp M0–M5 + zorm_server 四工具），无虚构。*
