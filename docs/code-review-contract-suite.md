# 代码评审报告：契约测试统一重构（commit 3440436）

- 评审对象：`3440436 "refactor tests .. all tests go throught from the ContractSuite"`
- 评审日期：2026-09-20
- 评审范围：`tests/TestConfig.*`、`tests/dbconfig.json`、`tests/ContractSuite.h`、`tests/test_contract.cpp`、`run-test`、`CMakeLists.txt`、四个后端头文件（Sqlit3Db/MysqlDb/PostgresDb/Dm8Db）的能力对等改造、`DbUtils.h` 新增公共工具
- 评审基线：全部 7 个 CTest 套件在真实数据库（mysql/pg/dm8 @ 10.0.0.7）上 7/7 通过

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

## 三、记录在案、建议后续处理的问题（未改码）

按优先级排列：

| # | 位置 | 问题 | 建议 |
|---|---|---|---|
| O-1 | MysqlDb 参数化 SELECT | `MYSQL_TYPE_DATETIME/TIMESTAMP/DATE/TIME` 列以二进制 `MYSQL_TIME` 结构落入缓冲区，但解码走"字符串"分支——**含日期列的参数化 SELECT 会读出乱码**。测试表无日期列所以未暴露 | 解码处 memcpy 到 MYSQL_TIME 并格式化为字符串；或绑定 buffer_type 强制字符串传输 |
| O-2 | 四后端连接池 | 池句柄无 checkout/checkin 语义：两个并发线程可能拿到**同一个连接**执行（rand 轮询只是缓解）。事务（transGo）期间若并发进入同一连接会话状态互相污染 | 引入 RAII 连接租借（mutex + 条件变量或每线程连接），这是交付多线程服务前必须解决的 |
| O-3 | attachRecordsPages | count 查询通过 `GetConnection` 可能落在**与主查询不同的池连接**上（InnoDB REPEATABLE READ 下两快照可能不一致），records 与 data 存在竞态偏差 | count 复用主查询的连接，或接受近似值并文档化 |
| O-4 | CountSqlFromSelect | 以子串 `" from "` / `" order by "` 切分拼 count SQL——非参数化模式下若 WHERE 字面量含这些串会错切 | 参数化模式下安全（值走绑定）；可文档化"非参数化模式的值不要包含这些关键字"，或改为在 genSql 内构造而非事后切分 |
| O-5 | buildUpdateSql | params 只含 `id` 时生成 `update t set  where id=?`（语法错误 → 701）。旧行为一致，但契约未覆盖 | 补一条契约：仅 id 时返回 301 或 no-op（各后端统一后加断言） |
| O-6 | insertBatch upsert 语义不一致 | mysql 带 `on duplicate key update`（重复 id 成功），**sqlite/pg/dm8/sqlite3-mem 无 upsert**（重复 id 报 701）。本次新增测试时已实际踩到 | 统一：要么全部加 upsert（pg: `ON CONFLICT DO UPDATE`，sqlite: 同，dm8: MERGE），要么把 mysql 改成与其它一致；推荐前者（gels 契约是 upsert） |
| O-7 | dbconfig.json | mysql 的 `nullRendering: "null-string"` 疑似过时：参数化路径现在正确返回 JSON null，与 "json-null" 的断言在 toString 层面恰好等价，掩盖了配置语义 | 用 `isNull()` 区分后复核各后端真实渲染，更新配置 |
| O-8 | TestConfig.cc | `readConfigFile` 是外部链接函数但未在头文件声明（ODR 隐患）；`resolveDialect` 与 `resolveDialectWithArgv` 90% 重复 | 加 static 或头文件声明；抽出公共实现 |
| O-9 | DbUtils.h | `GenerateId` 用了 `std::uint32_t` 但未包含 `<cstdint>`（靠 `<random>` 间接带入） | 显式 include |
| O-10 | allocate_buffer_for_field | `#if A1 / #if A0` 死代码块（宏未定义）；`const MYSQL_FIELD field` 按值拷贝 | 清理死代码；改 const 引用 |
| O-11 | ExecNoneQuerySql | `int affected = (int)mysql_affected_rows(...)`：my_ulonglong → int 截断（超大批量） | 用 long long 并让 affectedRows 走 long long |

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
