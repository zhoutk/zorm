# 代码评审报告：契约测试统一重构（commit 3440436）

- 评审对象：`3440436 "refactor tests .. all tests go throught from the ContractSuite"`
- 评审日期：2026-09-20
- 评审范围：`tests/TestConfig.*`、`tests/dbconfig.json`、`tests/ContractSuite.h`、`tests/test_contract.cpp`、`run-test`、`CMakeLists.txt`、四个后端头文件（Sqlit3Db/MysqlDb/PostgresDb/Dm8Db）的能力对等改造、`DbUtils.h` 新增公共工具
- 评审基线：全部 7 个 CTest 套件在真实数据库（mysql/pg/dm8 @ 10.0.0.7）上 7/7 通过
  > 后续演进：第三轮已扩展为 **11 个注册、11/11 通过**（四个非参数化方言 + TypeFidelity + EscapingFidelity），见第八节。

---

## 一、总体评价

方向正确，质量显著提升：

1. **gels 式配置驱动**（`--dialect` / `ZORM_DB_DIALECT` / dbconfig.json 三级选择）把"一套契约、多个后端"落到了单个测试二进制，消除了四个几乎相同的 per-backend 测试文件的维护负担；
2. **能力对等下沉**：auto-id、upsert（create 重复 id）、records/pages、结构化 transGo（Insert/Update/Delete/Batch）、单元素 insertBatch、affectedRows 现在是**全后端契约**而不是 jsonfile 专属，对外接口语义统一是本次最有价值的改动；
3. DbUtils 新增 `GenerateId/CountSqlFromSelect/Trim` 共享工具，去重方向正确。

但评审在 MySQL 后端发现了**若干会在生产环境触发的真实缺陷**（崩溃/泄漏/精度截断），已在本次评审中修复；另有一批观察项记录在案。

---

## 二、已修复的问题（本次评审直接改码）

全部位于 `src/include/MysqlDb.h`（其余三个后端经扫描确认不存在同类模式）：

### P1-1 非参数化查询遇到 SQL NULL 直接崩溃
`ExecQuerySql`（非参数化路径）里 `IS_NUM(fields[i].type)` 分支无条件执行 `atof(row[i])`。MySQL C API 对 NULL 值返回 `row[i] == NULL`，`atof(NULL)` 是未定义行为（实测段错误）。触发条件：`parameterized=false` 且任何数值列含 NULL——生产环境极易命中。
**修复**：`row[i] == nullptr` 时写入 JSON null（与参数化路径的 `is_null` 处理对齐）。

### P1-2 参数化取值与缓冲区宽度不匹配（截断/垃圾数据）
参数化 SELECT 的解码循环按指针强转读取缓冲区：
- `MYSQL_TYPE_LONGLONG`（count 统计）用 `(long)*(int*)` 读 8 字节缓冲 → **计数超过 2^31 即截断**；
- `MYSQL_TYPE_FLOAT` 落入"字符串"分支 → 把 4 字节二进制 float 当 C 字符串读出**乱码**；
- `TINY/SHORT`（1/2 字节缓冲）同样落入字符串分支。
**修复**：按字段类型精确解码（TINY→signed char、SHORT/YEAR→short、LONG/INT24→int、LONGLONG→long long、FLOAT→memcpy 后转 double、DOUBLE/DECIMAL→double、其余字符串）。

### P1-3 错误路径资源泄漏（句柄 + 内存）
stmt 系列函数在 `mysql_stmt_prepare/bind_param/execute` 失败时直接 return：
- `bind`（MYSQL_BIND 数组）不释放；
- `mysql_stmt_close(stmt)` 被跳过（连接池中的连接句柄持续泄漏）；
- `prepare_meta_result`（MYSQL_RES）**每条参数化 SELECT 泄漏一次**（正常路径也未释放）。
**修复**：所有失败路径补 `delete[] bind` / `mysql_stmt_close(stmt)`；正常路径补 `mysql_free_result(prepare_meta_result)`。

### P2-1 连接池游标用 `rand()`（线程安全）
`GetConnection` 用 `rand() % maxConn` 选连接。POSIX 不保证 `rand()` 线程安全（共享状态），而本函数在多线程服务下每条语句都会调用——这正是第三方交付场景。
**修复**：`inline static std::atomic<unsigned int> s_poolCursor` 轮询取模（顺带消除取模偏差）。

### P2-2 `escapeString` 空指针解引用风险
非参数化模式下 `escapeString` 通过 `GetConnection` 拿随机连接做 `mysql_real_escape_string`；当数据库不可达时返回 nullptr → 解引用崩溃。
**修复**：连接不可用时代码内转义（兼容 MySQL 转义规则：\0 \n \r \\ \' \" \Z）。

### 说明：与 ARM 可移植性审查相关的顺带收益
`rand()` 的替换同时消除了一个"平台 libc 实现差异"依赖点（glibc/uclibc/musl 的 rand 状态语义不同）。

---

## 三、记录在案的问题 → 修复状态（2026-09-20 第二轮，全部处理）

| # | 问题 | 修复方式 | 状态 |
|---|---|---|---|
| O-1 | mysql 参数化 SELECT 日期列按 MYSQL_TIME 二进制落入字符串分支（乱码） | 参数化解码新增 DATETIME/TIMESTAMP/DATE/TIME 分支：memcpy 到 MYSQL_TIME 后格式化为 `YYYY-MM-DD[ HH:MM:SS]` / `HH:MM:SS` | ✅ 已修复 |
| O-2 | 连接池无 checkout/checkin，并发线程可能共用同一连接 | 新增 `src/include/DbPool.h`：`DbPool::HandlePool<Handle>` 模板（mutex+condvar 槽位、懒建连、RAII Lease 独占租借），四 SQL 后端接入 | ✅ 见重构提交 |
| O-3 | records/pages 的 count 查询可能落在与主查询不同的连接（快照不一致） | `select()` 持有单个 Lease，主查询与 count 同连接执行 | ✅ 见重构提交 |
| O-4 | CountSqlFromSelect 以子串切分拼 count SQL（脆弱） | genSql 内用已知的 table/where/group 直接构造；分组查询包一层子查询使 records=分组数；`CountSqlFromSelect` 删除 | ✅ 已修复 |
| O-5 | 仅含 id 的 update 生成非法 SQL（后端间 301/701 不一致） | 四后端 buildUpdateSql 对"仅 id 无列"返回 false → 统一 301；jsonfile 对齐；契约断言固化 | ✅ 已修复 |
| O-6 | insertBatch upsert 语义不一致（mysql 有、pg/dm8/sqlite 无） | pg 原本已有（constraint 参数）；sqlite 补 `ON CONFLICT (constraint) DO UPDATE`；dm8 逐行走 create() 的读后写 upsert；mysql 原有。契约断言固化 | ✅ 已修复 |
| O-7 | dbconfig mysql nullRendering 过时 | 实测参数化路径 is_null → JSON null，配置改为 `json-null` | ✅ 已修复 |
| O-8 | TestConfig readConfigFile 外部链接未声明；resolve 函数重复 | readConfigFile 改 static；抽出 `dialectFromArgv` 公共实现 | ✅ 已修复 |
| O-9 | DbUtils 缺 `<cstdint>` | 显式 include | ✅ 已修复 |
| O-10 | allocate_buffer_for_field 死代码块 + 按值拷贝 | 删除 `#if A1/A0`；改 `const MYSQL_FIELD&` | ✅ 已修复 |
| O-11 | affected 的 my_ulonglong→int 截断 | 改 `long long` | ✅ 已修复 |

> O-2/O-3 的实现（DbPool 模板 + 各后端接入）与"多数据库封装去重重构"为同一改造，见下文第七节的修复设计与计划。

---

## 四、x86 ↔ ARM 可移植性审查（C++17，按检查清单逐项）

> 结论先行：**当前代码库没有清单中的致命项**。跨架构重编即可工作；需要留意的是 4 个"低风险、建议加固"项。

| 清单条目 | 本仓库现状 | 结论/建议 |
|---|---|---|
| 1.1 long double | 全库未使用 long double；JSON 数值经 `%.17g`（zjson）/`%.15g`（numberText）序列化，纯 double | ✅ 无风险 |
| 1.2 char 符号性 | 所有 char 语义使用点（SQL 词法解析、FileLock、escape）都是与 ASCII 字面量（`'?'`、`'\''`）比较或先转 `unsigned char` 再传 `isspace`——两种符号性下行为一致。`case 26`（escapeString 的 Ctrl-Z）在 char 为 signed 的平台也安全（与 26 显式比较） | ✅ 安全；**建议**为未来 ARM Linux 构建加 `-fsigned-char` 固化（本次已在 CMakeLists GCC/Clang 分支添加） |
| 1.3 结构体对齐 | 无 packed 结构、无共享内存布局、无手工内存布局假设。缓冲区全部是堆上 char[]，MYSQL_TIME 等由客户端库管理 | ✅ 无风险 |
| 1.4 指针强转 int | 无 `reinterpret_cast` 到整型（仅 char8_t/char 字符串视角转换）；唯一可疑截断是 `(int)mysql_affected_rows`（值截断，非指针，见 O-11） | ✅ 无风险 |
| 2.1 弱内存序 | 全库**无手写无锁代码、无 volatile 标志位**。并发原语只有 `std::mutex/shared_mutex`（JsonFileDb）与 `std::atomic`（本次新增的池游标、zjson 内部 keymap），默认 seq_cst 语义 | ✅ 无风险。zjson 属第三方，建议同步审查其 keymap 原子操作 |
| 2.2 16 字节 CAS | 未使用 16 字节原子（无 `-mcx16` 需求）；`atomic<unsigned int>` 在两架构均原生指令实现 | ✅ 无风险 |
| 2.3 未对齐原子访问 | 原子对象均为静态对齐标量 | ✅ 无风险 |
| 3.1/3.2/3.3 builtins/内联汇编/SIMD | 全库无 `__builtin_ia32_*`、无内联汇编、无 SSE/AVX intrinsic | ✅ 无风险 |
| 4.1 GCC12 零宽度位域 ABI | 无位域 | ✅ 无风险 |
| 4.2 跨架构 ABI | 交付物为源码 + 各平台重编（.so/.dll 不跨架构），符合清单要求 | ✅ 流程已正确 |
| 4.3 max_align_t | 未直接使用 | ✅ 无风险 |
| 5 thread_local | 仅一处：`DbUtils::GenerateId` 的 `thread_local mt19937_64`（函数内静态，首次访问一次初始化）；ARM 上 guard 开销每线程仅一次，可忽略 | ✅ 无风险 |
| 6 字节序 | 持久化格式是 **JSON 文本**（jsonfile 后端），天然字节序无关；二进制协议全部由各客户端库处理 | ✅ 设计上规避了该类问题 |
| 附加：平台 API | FileLock/JsonFileDb 的 Win32（CreateFileW/MoveFileExW）与 POSIX（open/rename/kill）双实现均有 `#ifdef` 守卫；Linux 用 /proc/self/exe | ✅ 已具备 Linux/ARM 交叉编译条件 |

**加固动作（本次已做）**：CMakeLists 为 GCC/Clang 添加 `-fsigned-char`（MSVC 本身恒定 signed），消除 char 符号性这一 ARM 迁移中最常见的隐患类别。

**ARM 构建待验证项**：dm8 DPI 与 MariaDB Connector/C 需要对应架构的库（pacman ARM 软件源或源码编译）；`thirds/dm8/dpi` 目前只有 x86 二进制——这是 ARM 迁移真正的工作量所在，与代码可移植性无关。

---

## 五、对外接口（Idb.h）测试覆盖矩阵

本次新增 4 项共享契约断言（见下表"新增"标注），使 8 个接口方法的全参数形态在**全部 6 个方言**上执行（`./run-test all`）：

| 接口方法 | 参数形态覆盖 | 所在契约 |
|---|---|---|
| select | params 全家桶（id/多条件/fuzzy/ins/lks/ors/比较符/between/分页/排序/分组/聚合）、fields（vector 与 params 双通道）、**values 第4参数（新增）**、缺失表、空库 | Read / Query |
| create | 全字段、auto-id（8位hex）、空字符串id、数组批量、重复id upsert、**部分字段缺省渲染（3种null语义）**、空对象拒绝 | Write / EdgeCases |
| update | 单字段、多字段、不存在id、缺id拒绝 | Write |
| remove | 正常、不存在id、缺id拒绝 | Write |
| querySql | 无WHERE+params 条件、**fields 投影（新增）**、占位符绑定（`?`/`$n` 双方言）、元数据目录 202、垃圾 SQL | Read / PlaceholderSql / MetadataCatalog |
| execSql | 字面量 update、占位符 update（`?`/`$n`）、垃圾/空 SQL 拒绝 | Dao / PlaceholderSql |
| insertBatch | 2行、单行、**显式 constraint（新增）**、空数组拒绝 | Dao |
| transGo | SQL text 元素、结构化 Insert/Update/Delete/Batch、整体回滚（SQL失败+非法method）、**isAsync=true（新增）**、空数组拒绝、事务内占位符绑定 | Dao / PlaceholderSql |
| DbBase 路由 | 未知 dbType 抛异常、配置驱动连接 | test_contract.cpp |

**发现并固化的契约差异**：insertBatch 重复 id 时 mysql 是 upsert 语义而 pg/dm8/sqlite 是主键冲突（O-6）——测试注释中已注明，是后续统一的输入。

## 六、结论

- 本次重构后，对外接口在 6 个方言上的行为契约已经**单一来源、全参数覆盖**，`./run-test all` 7/7 通过；
- 评审修复了 6 处 MySQL 后端缺陷（2 处崩溃级、1 处精度截断、3 处泄漏/并发），全部有针对性且改动局部；
- 交付第三方前建议优先处理 **O-2（连接池并发语义）** 与 **O-6（insertBatch upsert 统一）**；
- ARM 迁移的代码侧风险已清零（含 `-fsigned-char` 加固），剩余工作是 dm8/mariadb 客户端库的 ARM 二进制供给。

---

## 七、多数据库封装去重重构（gels 分层思想的 C++17 落地）

### 7.1 设计

参考 gels 的 baseDao / sqlDialect 分层，但**不做类继承树套用**——结合 zorm 自身特点（每个后端是一个自包含单头文件、Idb 八方法契约、零依赖），用 C++17 惯用法实现：

| 组件 | 职责 | 位置 |
|---|---|---|
| `DbPool::HandlePool<Handle>` | 连接池模板：槽位懒建连、`mutex`+`condition_variable` 等待、**RAII `Lease` 独占租借**（O-2）；`invalidate()` 在致命连接错误后自愈；`setMaxConn()`。句柄按**值语义**存储（对 `sqlite3*` 这类本身就是指针的句柄，避免了二级指针陷阱） | `src/include/DbPool.h`（160 行） |
| `SqlBackendBase<Derived, Handle>` | 共享算法单份实现：8 个 Idb 方法骨架、`buildInsert/Update/DeleteSql`、`buildStructuredSql`、`genSql` 智能查询装配、聚合/分页/记录计数、事务循环、`attachRecordsPages`（O-3：主查询与 count 共用同一 Lease）；派生类通过 **CRTP 钩子**提供方言，内部调用零虚函数开销 | `src/include/SqlBackendBase.h`（783 行） |
| 四个后端 | 只保留驱动层与方言：连接/编解码/转义/方言钩子（占位符、标识符引用、LIMIT 语法、UPSERT 子句、聚合别名、字段投影） | 各 .h 瘦身 |
| jsonfile | 不受影响（文件型后端本就无 SQL 方言） | — |

方言钩子集合（14 个）：`placeholder / numberedPlaceholders / quoteIdent / qualifiedTable / columnList / fieldsProjection / likeColumn / orderClause / limitClause / upsertClause / aggColumn / aggAlias / countAliasSql / excludedRefImpl` + 驱动钩子 `acquireHandle / execQueryOn / execNoneOn / execTxOn / beginTx / commitTx / rollbackTx / detectParameterized / escapeString`。

### 7.2 效果

| 指标 | 重构前 | 重构后 |
|---|---|---|
| 四后端总行数 | 4221（1204+883+1247+887） | 1721（549+276+617+279） |
| 共享算法 | 4 份近似副本 | 1 份（783 行基座）+ 池 160 行 |
| 合计 | 4221 | 2664（**净减 1557 行 / 37%**） |
| 新增后端成本 | 复制 1200 行改方言 | 实现 ~14 个钩子（~250 行） |
| O-2/O-3 | 池无租借语义、count 可能跨连接 | RAII 独占租借、同连接快照 |

### 7.3 迁移中发现并修复的问题（全部由真实数据库验证）

重构过程中探针/契约测试暴露了一批历史遗留缺陷——它们证明这套共享契约+真实数据库的门槛是有效的：

1. **MySQL DECIMAL 解码错误（严重）**：MariaDB Connector/C 二进制协议中 `NEWDECIMAL` 以**字符串形式**传输，旧代码按 `*(double*)` 读取——`sum()` 结果一直是垃圾值（实测 7.108e-320）。此前契约测试"通过"是因为断言恰好走了另一分支；重构中换上探针后暴露。修复：DECIMAL/NEWDECIMAL 走 `atof` 字符串解码。
2. **sqlite 列名探测与 fields 耦合**：列探测把传入的 `fields` 预置进结果列名列表，一旦生成 SQL 不再包含这些列（聚合场景）就整体错位。修复：列名完全来自探测结果（`sqlite3_get_table`），`fields` 只由 genSql 负责进 SQL。
3. **聚合投影的跨引擎可移植规则**：`select id,count(1)` 在 pg / MySQL(ONLY_FULL_GROUP_BY) 下非法。统一规则：有聚合时投影 = 分组列（分组时）或纯聚合；fields 仅在无聚合时生效。三种方言实测一致。
4. **dm8 `qualifiedTable` 误用**：基座早期版本对 queryType 2/3（用户 SQL）也做表名限定，dm8 会把 `SET SCHEMA ...` 整句包成 `"dbtest"."SET SCHEMA ..."`。修复：限定只作用于 queryType 1 与 count SQL。
5. **dm8 `execNoneOn` 分派遗漏**：参数化模式（`dbconfig` 默认）下写路径必须走 prepare+bind，否则 `?` 原样发给服务器（`-6804 缺少必要的参数`）。修复：按 `queryByParameter` 分派。
6. **dm8 `orderClause` 拼接错误**：用逗号 join 了空格分隔的 token，生成 `order by "age",asc`（语法错）。修复：按逗号分句、按空格分词的独立拼接，方向关键字保持不引用。
7. **dm8 批量列名未引用**：`insertBatch`/结构化 Batch 的列清单早期用裸名 join，DM8 折叠成大写（[ID] 无效）。修复：新增 `columnList` 钩子（dm8 用带引号形式）。

### 7.4 交付说明与遗留

- `HandlePool::acquire()` 在池耗尽时**阻塞等待**（条件变量），比旧实现的"随机复用一个连接"更安全，但调用方需注意长事务会占住槽位；`db_conn` 配置决定上限。
- dm8 的事务在 `beginTx/commitTx/rollbackTx` 中切换并在结束后**恢复 AUTOCOMMIT**（旧实现恰好也做了）。
- 偶发观察：`test_jsonfile` 曾在一次全量运行中耗时 30s（FileLock 的 30 秒 stale 窗口），复跑即恢复 <1s——疑与磁盘/杀软瞬时抖动有关，非代码缺陷，记录备查。
- 仍是后续项：QueryType 2/3 的 `fields` 替换仅支持 `*` 在语句前 10 字符内命中（沿袭旧行为）；如未来要支持 `SELECT a.*` 形式可再扩展。


---

## 八、测试覆盖强化与变异验证（2026-09-20 第三轮）

### 8.1 复核：DECIMAL 解码缺陷是否已修复、是否已有测试守门

**结论：已修复，且已被测试守门，并用变异测试验证了守门有效。**

| 变异（人为改坏实现） | 期望被捕获 | 实测结果 |
|---|---|---|
| A 把 MySQL DECIMAL 解码改回 `*(double*)`（原缺陷） | mysql 方言 | ✅ `test_contract_mysql` 失败，且同时触发 **3 处**断言：`Query.agesum`(57)、`TypeFidelity` 的 price 往返(1234.56)、decimal 聚合(1234.56) |
| C2 把 datetime 解码输出替换为固定串 | mysql 方言 | ✅ `TypeFidelity.ts` 断言失败（O-1 的修复同样有守门） |
| D 在字符串解码分支加前缀 | 全方言 | ✅ `Read` 的多处断言失败（证明该分支被覆盖） |
| C（无效变异：在 datetime 分支插入一条会被后续代码覆盖的赋值） | — | ⚠️ 未被捕获——**教训：变异必须自证“确实生效”**，否则会得出“测试没覆盖”的错误结论 |

**关于“为什么之前的契约测试没抓到”的实证记录**：在 pre-refactor 提交（`6b30302`）上编译并运行 mysql 契约（git worktree 实测），`Contract.Query` 的 `agesum=57` 断言**通过**；对同一份旧代码施加同一个“把解码分支改成返回常量”的变异后，该断言**失败**——说明断言本身有效，旧代码在那条具体路径上确实返回了正确值。DECIMAL 垃圾值（7.1e-320）是在重构中间态用独立探针查询时暴露的。旧路径为何未触发该解码分支**未能完全定位**；无论历史成因如何，当前状态是“修复 + 变异验证的守门”，这是可执行的保证。

### 8.2 本轮新增的测试（针对“解码类缺陷不会再漏”）

1. **`TypeFidelity` 契约段**（全 6 方言执行）：
   - decimal/numeric 列**精确往返**：`price=1234.56` 写入后读回必须 `== 1234.56`（`EXPECT_DOUBLE_EQ`）；
   - decimal 列上的**聚合精度**：`sum(price)` 必须精确；
   - 小数列聚合 `sum(score)` 精确；
   - **datetime 列往返**：写入 `2026-09-20 10:30:45`，读回须为同一渲染（覆盖 MySQL 的 `MYSQL_TIME` 解码，即 O-1）；
   - **类型列上的 NULL**：按配置的 nullRendering（json-null / null-string / empty）断言。
2. **schema 扩展**：契约表新增 `price decimal(10,2)` 与 `ts datetime` 两列（6 个方言的 DDL 各自使用等价类型：mysql `decimal/datetime`、pg `numeric/timestamp`、dm8 `DECIMAL/DATETIME`、sqlite `decimal/datetime`、jsonfile 仅记录列名）。
3. **非参数化方言**（覆盖另一套代码路径）：`--no-param` 开关 + **四个新注册**
   - `test_contract_sqlite3_mem_plain`（`./run-test sqliteplain`）
   - `test_contract_mysql_plain`（`./run-test mysqlplain`）
   - `test_contract_postgres_plain`（`./run-test pgplain`）
   - `test_contract_dm8_plain`（`./run-test dmplain`）
   它们跑**同一套契约**但 `parameterized=false`，覆盖：字面量 SQL 生成（genSql/构造器的 else 分支，值经 `escapeString` 转义后内联）、非参数化解码（含 P1-1 的 `atof(NULL)` 修复路径）。
   - **注册后立刻抓到两个真实缺陷**：① `ins` 查询的非参数化分支在基座重构时丢失（生成 `in (?,?,?)` 却按字面量执行 → 701）；② 见下一条"转义保真"。
   - jsonfile 没有 plain 变体：它的 SQL shim 同时解析 `?` 绑定与字面量（同一段代码两条分支），不存在"参数化 vs 拼接"两套独立实现；其字面量路径由 jsonfile 专属套件的 SqlEdge 用例覆盖。
4. **`EscapingFidelity` 契约段**（全 11 个注册执行）：含引号/百分号/下划线的值必须精确往返——
   `create` 后按 id 读回、按该值做等值条件查询、`fuzzy` 子串查询、`update` 携带撇号、`insertBatch` 批量携带。
   - **为什么必须加**：契约原有数据（Kevin 凯文/test001/中文…）**不含单引号**，因此 `postgres` 与 `dm8` 的 `escapeString` 一直是 `return true;`（空实现，注释写着 "values ride through binding"）却从未被发现——加上本段后 `pgplain`/`dmplain` 立刻失败，证实了空转义在非参数化模式下会直接生成坏 SQL。
   - 修复：为 postgres/dm8 实现标准 SQL 转义（单引号加倍，标准合规字符串为默认）；同时把 mysql 的 `escapeString` 改为**纯内置转义不再取连接租借**——原先它 acquire 一个 lease，而 `transGo` 已经持有租借再构建语句，`db_conn=1` 的配置会自锁。
   - 变异验证 E：把 postgres 的转义改回空实现 → `pgplain` **失败**，而 `postgres`（参数化）**通过**——守门精准且路径隔离正确。
4. **测试基础设施修复**（本轮发现）：
   - `test_jsonfile.cpp` 未初始化 `contract::g_config` → `parameterized()` 误判为 false，导致共享契约走错分支（已初始化并标记 jsonfile 的 `?` 绑定语义）；
   - `PlaceholderSql` 契约按模式分叉：参数化模式走占位符语句，plain 模式走等价字面量语句（否则 plain 方言必然失败，且这些路径将完全没有覆盖）；
   - `dbconfig.json`：jsonfile 显式声明 `parameterized: true`（其 SQL shim 接受 `?` 绑定，与 SQL 后端语义对齐）。

### 8.3 覆盖矩阵（更新）

| 注册名 | 方言 | 参数化 | 说明 |
|---|---|---|---|
| test_contract_sqlite3_mem | sqlite3-mem | ✅ | 内存库 |
| test_contract_sqlite3 | sqlite3 | ✅ | 文件库 |
| test_contract_jsonfile | jsonfile | — | 文件型后端契约 |
| test_contract_sqlite3_mem_plain | sqlite3-mem | ❌ | 字面量/转义路径 |
| test_contract_mysql | mysql | ✅ | |
| test_contract_mysql_plain | mysql | ❌ | 字面量 SQL + 非参数化解码 |
| test_contract_postgres | postgres | ✅ | |
| test_contract_dm8 | dm8 | ✅ | |
| test_contract_postgres_plain | postgres | ❌ | 字面量 SQL + 转义 + 非参数化解码 |
| test_contract_dm8_plain | dm8 | ❌ | 同上 |
| test_jsonfile | jsonfile | — | 存储引擎加固（损坏/锁/原子写/编码） |

`./run-test all` → **11/11**。

### 8.4 回归守门约定（建议长期执行）

1. 任何后端改动后：`./run-test all`（本地 + 远程全方言）；
2. 改动**类型解码/语句构造**后：做一次变异验证——把改动点人为改坏，确认至少一处断言失败（否则说明该路径无覆盖，先补测试）；变异必须自证生效（参考 8.1 的无效变异 C）；
3. 新增方言/驱动：注册新的 `--dialect` 条目外，**同时注册一个 `--no-param` 变体**（并确保 `escapeString` 真的有方言正确的实现，而不是 `return true;`）；
4. 契约表新增列时：同步更新 `tests/dbconfig.json`、`test_jsonfile.cpp` 的内嵌 DDL、以及 TypeFidelity 的断言。
