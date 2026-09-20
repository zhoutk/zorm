# JsonFileDb 设计思想与测试详解

> 本文档解释 zorm 的 JSON 文件型后端（`src/backends/JsonFileDb.cpp` + `src/backends/JsonFileDb.h`）的
> 设计思想、它区别于 SQL 后端的特别之处，以及 `tests/test_jsonfile.cpp` 加固测试的详细内容。

---

## 1. 它是什么

`JsonFileDb` 是 zorm 里一个**纯 C++17、零外部依赖**（仅依赖 ZJSON）的 `Idb` 后端：
整个"数据库"就是**一个 JSON 文件**。

```jsonc
// 文件布局：一个 JSON 数组，每个元素是一张表
[
  {
    "table": "routes",
    "columns": ["id", "name", "age", "score"],
    "rows": [
      { "id": "a1b2c3d4", "name": "Kevin 凯文", "age": 18, "score": 99.99 },
      ...
    ]
  },
  ...
]
```

通过 `DbBase("jsonfile", {"connString": "path/to/data.json"})` 使用，走统一的 `Idb.h` 接口。
它不依赖任何数据库服务器 —— 适合嵌入式、本地工具、快速原型等场景。

---

## 2. 设计思想（核心原则）

### 2.1 一套接口，行为与 SQL 后端对齐
JsonFileDb 实现完整的 `Idb.h` 接口，并在能力上与 SQL 后端对齐（本仓库做了大量"功能下沉"工作）：
- `create()` 缺 id 时自动生成 8 位 hex id；已有 id 时 **upsert**（覆盖写）
- `remove()` 缺 id 返回 301
- `insertBatch()` 支持单元素
- `select()` 返回 `records` / `pages`
- 结构化 `transGo`（Insert / Update / Delete / Batch）
- 弱类型 JSON 值（`"20"` == `20`、null/boolean 原生保留）

### 2.2 内存镜像 + 惰性加载
- 文件首次被访问时一次性读入内存（`store_`），之后读写都针对内存镜像。
- 内存镜像由 `shared_mutex` 保护：**并发读、独占写**。

### 2.3 写前重载（write-through，失败不污染内存）
> 核心不变量：**写操作先重读磁盘 → 在内存镜像上修改 → 原子写回；若修改或写盘失败，内存回滚到之前状态。**

这条不变量保证了"失败的写永远不会让内存状态领先于磁盘"，是数据一致性的基石。

### 2.4 跨进程文件锁（`<path>.lock`）
- 写者先获取同路径的锁文件（`FileLock`），再执行读-改-写。
- 锁文件记录了 pid / host / app / 时间戳；陈旧锁（死进程或超时窗口）会被回收。
- **锁顺序**：文件锁在 `storeMutex_` 之前获取 —— 等待其他进程写锁时**不阻塞本地读者**。

### 2.5 原子写 + 临时文件
- 写盘用"写临时文件 → 原子替换"（`<db>.tmp.<pid>.<hex>`），避免写一半损坏主文件。
- 构造实例时会**清扫**死进程遗留的临时文件（`sweepStaleTempFiles`）。

### 2.6 损坏保护（不假装数据库为空）
- 文件损坏 / 不可读时：读取返回 `701`（数据库操作失败），并**备份损坏文件**（`<db>.corrupt.*`），而不是静默返回空结果。
- 修复文件后，下一次写入重新加载成功，**自动自愈**，清空失败状态。

### 2.7 每表 id 索引（O(1) 增删改）
- `tableIdIndex_`：`表名 -> (id -> 行下标)`，按 id 的 create/update/remove 是 O(1)。
- 删除用 **swap-pop**：把最后一行换到被删位置（O(1) 维护索引），代价是行顺序不稳定（这是文档化的行为）。

### 2.8 每路径单例（createShared）
- `createShared(path)`：同一路径返回**同一个**共享实例（weak_ptr 注册表）。
- 所有引用释放后注册表条目过期，下一次调用创建新实例（测试中用于"重启"数据库）。

### 2.9 SQL 文本 shim（模拟 SQL 方言）
为了和 SQL 后端在 `querySql` / `execSql` / `transGo` 上行为一致，JsonFileDb 内置了一个小型 SQL 解析器：
- `?` 占位符绑定、`??` 表名占位符
- `'...'` 字面量（`''` 转义）、`null` / `true` / `false`、数字字面量
- `CREATE TABLE` / `DROP TABLE` / `INSERT INTO` / `UPDATE` / `DELETE FROM` / `BEGIN|COMMIT|ROLLBACK`（后三者为 no-op shim）
- `sqlite_master` / `information_schema.tables` 元数据查询 shim（回答"表是否存在"）

---

## 3. 特别之处（与 SQL 后端的本质区别）

| 维度 | SQL 后端 (mysql/pg/dm8/sqlite3) | JsonFileDb |
|------|-------------------------------|------------|
| 数据可靠性 | **服务器**保证（WAL、事务、崩溃恢复） | **自己**保证（原子写、锁文件、内存回滚、损坏备份） |
| 并发控制 | 服务器事务/锁 | `shared_mutex`（进程内）+ 文件锁（跨进程） |
| 事务 | 服务器 `BEGIN/COMMIT/ROLLBACK` | 内存镜像修改 + 失败回滚（`writeWithLock`） |
| 字符集校验 | 服务器按列类型处理 | 自己用 `ParseJsonStrictUtf8` 校验，非法 UTF-8 视为损坏 |
| 元数据 | 真实 catalog（INFORMATION_SCHEMA 等） | 模拟 shim（只回答表存在性） |
| 值类型 | 强类型（列类型约束） | 弱类型 JSON（`"20"`==20，保留 null/boolean） |
| 行顺序 | 稳定（按主键/物理顺序） | swap-pop 后不稳定（文档化） |
| 崩溃时数据 | 服务器恢复 | 原子替换保证主文件完整；临时文件由下个实例清扫 |
| 持久化 | 服务器管理 | 文件本身即持久化；新实例重新读文件 |

一句话：**SQL 后端把可靠性外包给服务器，JsonFileDb 必须自己实现"数据库级别的可靠性"。**
这正是它拥有独立"加固测试"的根本原因。

---

## 4. 测试详解（`tests/test_jsonfile.cpp`）

该文件包含**两部分**：

### 4.1 共享契约套件（和其他后端相同）
文件末尾 `ZORM_CONTRACT_TESTS()` 实例化 `ContractSuite.h` 的通用套件
（Read / Write / Query / Dao / EdgeCases / MetadataCatalog / PlaceholderSql），
通过 `DbBase("jsonfile", ...)` 走统一接口 —— 证明 jsonfile 满足 `Idb.h` 契约。
测试环境用 `JsonFileContractEnv` 重写了几个方言钩子：
`nullRendering="json-null"`、`supportsWherePlaceholders=false`、`autoCreateTables=true`。

### 4.2 jsonfile 独有加固测试（重点）

| # | 测试 | 验证什么 | 对应的设计原则 |
|---|------|---------|--------------|
| 1 | `JsonFileOnlyWriteContract` | swap-pop 删除顺序、auto-id、upsert、affectedRows、单元素 insertBatch、空输入拒绝 | §2.7 / §2.1 |
| 2 | `ResponseShapeContract` | `records`/`pages` 响应形状（6 行、分页、空结果） | §2.1 |
| 3 | `QuerySqlContract` | sqlite_master / information_schema shim、纯 select 路由、`select ... where` 文字子句不支持 | §2.9 |
| 4 | `SqlEdgeContract` | 事务 shim、字面量转义、`?`/`??` 占位符、drop table、拒绝非法 SQL、`update/delete` 无 WHERE 拒绝 | §2.9 |
| 5 | `HandcraftedStoreContract` | 手写 JSON 文件（无 CREATE TABLE）加载后 id 索引可用、可增删改 | §2.2 / §2.7 |
| 6 | `TransGoStructuredContract` | 结构化事务 Batch/Update/Delete、错误回滚、空数组拒绝 | §2.1 / §2.3 |
| 7 | `ValueTypesContract` | null/boolean/数字字符串、`"20"`==20 跨类型、sum 跳过 null/boolean | §3 弱类型 |
| 8 | `QueryValidationContract` | 聚合参数形状校验、范围比较、字段投影（含 params 内 `fields` 扩展）、空对象 remove 拒绝 | §2.9 |
| 9 | `FileHygieneContract` | 空文件=空库；清扫死进程 `.tmp`；保留活进程 `.tmp`；损坏文件经 querySql 报 701 | §2.5 / §2.6 |
| 10 | `PersistenceContract` | 新实例读到旧实例写入；相对路径解析 | §2.2 / §2.8 |
| 11 | `SharedInstanceContract` | `createShared` 同路径单例；引用释放后重建新实例 | §2.8 |
| 12 | `MemoryContract` | **文件被外部写坏 → 写失败，但内存数据不丢，读继续返回旧数据** | §2.3 核心不变量 |
| 13 | `EncodingContract` | 非法 UTF-8 = 损坏（701 + 备份 + 拒写）；修复后自愈；有效 UTF-8 往返；文件是合法 JSON 数组 | §2.6 / §2.1 |
| 14 | `FileLockTest` | 锁文件所有权、解锁删除、外来锁保留、陈旧锁回收、活锁超时 | §2.4 |
| 15 | `WriteWaitTest` | **等锁的写者不阻塞读者**（读 < 200ms、写 > 300ms） | §2.4 锁顺序 |
| 16 | `DbBaseTest` / `DefaultPathTest` | DbBase 拒绝未知类型；默认存储路径 = 可执行文件目录 | 接入层 |

### 4.3 测试基础设施
- **每个测试用独立的 scratch 文件**（`zorm_jsonfile_<tag>_<pid>.json`），
  绝不指向开发者真实数据文件；`removeDatabaseFiles` 清理主文件 + 锁文件 + 临时 + 损坏备份。
- `createShared` + 引用释放模拟"数据库重启"，验证持久化。
- 直接写坏文件（`writeFile(path, "{ broken")`）模拟外部破坏，验证容错。

---

## 5. 运行方式

```bash
./run-test json     # 契约套件 + 加固测试（两者都跑）
# 等价于 ctest -R "test_contract_jsonfile|test_jsonfile"
./run-test local    # sqlitemem + sqlite + json 契约 + json 加固
./run-test all      # 全部 7 个 ctest 注册项
```

> 注意：`json` 参数会同时运行 `test_contract_jsonfile`（通用契约）和
> `test_jsonfile`（文件型加固）—— 因为后者是这个后端"像不像数据库"的关键证明。

---

## 6. 维护提示（动 JsonFileDb 时请保持）

- **写前重载**：写操作先重读磁盘，失败回滚 —— 别改成"直接改内存"。
- **锁顺序**：文件锁 → `storeMutex_`；等锁不能阻塞本地读者。
- **损坏 = 错误**：读损坏文件返回 701 并备份，绝不假装空库。
- **原子写**：临时文件 + 替换；新实例清扫死进程残留。
- **单例**：`createShared` 每路径一个实例。
- **id 索引**：增删改保持 O(1)（swap-pop 是文档化的行为）。
- 改动后跑 `./run-test json`，加固测试全绿才算安全。

---

## 文本 vs JSON 文档：`Json::str`

ZJSON 的 `Json(text)` / `add(key, raw_text)` 对以 `{` 或 `[` 开头的文本会**按 JSON 解析**
（`Json("[1,2]")` 得到数组）。这对"值是文档"的场景很方便，但对"值恰好长这样的一段文本"
（配置片段、SQL 字面量、形如 `{"a":1}` 的业务字段）会造成**静默的类型改变**。

因此本项目约定：

- **要表达文本，一律用 `Json::str(text)`**——它构造 String 节点，绝不解析；
- `JsonFileDb` 内部所有"值语义"的构造（SQL 字面量、行 id、查询条件值、元数据表名）
  以及四个 SQL 后端的读取解码，都已改用 `Json::str`；
- 裸字符串写法（`Json("[1,2]")`、`Json{{"k","[1,2]"}}`）**保持既有嗅探语义**不变。

回归测试：共享契约的 `EscapingFidelity`（含 `[1,2] {"a":1}` 与合法 JSON 文本 `[1,2]`
的往返与条件查询）在全部 11 个注册上执行。
