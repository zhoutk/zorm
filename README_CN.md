# zorm  &emsp;&emsp;  [English](README.md)  

## 介绍
我们通用的ORM，基本模式都是想要脱离数据库的，几乎都在编程语言层面建立模型，由程序去与数据库打交道。虽然脱离了数据库的具体操作，但我们要建立各种模型文档，用代码去写表之间的关系等等操作，让初学者一时如坠云雾。我的想法是，将关系数据库拥有的完善设计工具之优势，来实现数据设计以提供结构信息，让json对象自动映射成为标准的SQL查询语句。只要理解了标准的SQL语言，我们就能够完成数据库查询操作。更进一步，可以使用关系数据库的视图和存储过程来处理表之间的关系，并且在应用层使用一个映射（Zrest - next prj will realize），就可以仅仅使用Zorm和Json来完成所有的数据库操作。

## 相关项目
本项目依赖 本人的 另一个项目 Zjson，此项目提供简洁、方便、高效的Json库。该库使用方便，是一个单文件库，只需要下载并引入项目即可。具体信息请移步 [gitee-Zjson](https://gitee.com/zhoutk/zjson.git) 或 [github-Zjson](https://github.com/zhoutk/zjson.git) 。

## 项目名称说明
本人姓名拼音第一个字母z加上orm，即得本项目名称zorm，没有其它任何意义。我将编写一系列以z开头的相关项目，命名是个很麻烦的事，因此采用了这种简单粗暴的方式。

## 设计思路 
ZORM 数据传递采用json来实现，使数据标准能从最前端到最后端达到和谐统一。此项目目标，不但在要C++中使用，还要作为动态链接库与node.js结合用使用，因此希望能像javascript一样，简洁方便的操作json。所以先行建立了zjson库，作为此项目的先行项目。设计了数据库通用操作接口，实现与底层实现数据库的分离。该接口提供了CURD标准访问，以及批量插入和事务操作，基本能满足平时百分之九十以上的数据库操作。项目基本目标，支持Sqlite3,Mysql,Postges,达梦8 四种关系数据库，另含 JsonFile 文件型存储后端，同时支持windows、linux和macOS。

## 项目特点
- **一个接口，五个后端**：`ZORM::Idb` 是公共契约，sqlite3、mysql、postgres、dm8（达梦）与 jsonfile（纯 C++ 文件存储）以完全一致的语义实现它。运行期切换数据库 = 换一个构造参数。
- **以静态库（zormlib）形式构建**：把 `src/include` 加入头文件搜索路径，include `DbBase.h`，链接 zormlib 即可使用——驱动头文件（mysql.h、DPI.h 等）不会泄漏到使用方代码中。整个库只编译一次，可执行程序与测试直接链接 zormlib。
- **智能查询**：查询参数就是普通 Json，自动装配成标准 SQL（分页、排序、模糊/in/or 匹配、聚合、分组）——没有模型类，也没有查询构造器 DSL。
- **upsert 语义对齐**：`create()` 在 id 缺失时自动生成 8 位十六进制 id；提供了 id 且已存在时覆盖写入——五个后端行为完全一致。
- **真正的连接池**：每个 SQL 后端的连接都在独占 RAII 租约之后出借（一条连接同一时刻只属于一条语句/一个事务，借出阻塞等待，连接故障后自愈）。
- **配置驱动的契约测试**：一套测试跑全部六个后端，切换被测数据库就是切换一个配置值。

## 项目进度
现在已经实现了基本目标的所有功能。  
我选择的技术实现方式，基本上是最底层高效的方式。sqlit3 - sqllit3.h（官方的标准c接口）；mysql - c api（MINGW 下用 pacman 的 MariaDB Connector/C，OpenSSL 3 后端支持 TLSv1.2/1.3，MSVC 下仍用第三方目录里的 MySQL Connector C 6.1）；达梦8 - dpi；postgres - c api(pgsql14)；pqxx分支实现了libpqxx7.7.4的封装，linux和macos上运行正常，windows上运行有问题，待解决。

### 架构

```
                ┌──────────────────────────────┐
   使用方代码 →  │  DbBase (工厂 + 门面)         │   src/include + src/DbBase.cpp
                └──────────────┬───────────────┘
                               │ Idb（8 个方法，签名冻结）
        ┌──────────────────────┼──────────────────────────┐
        ▼                      ▼                          ▼
┌───────────────────┐  ┌───────────────────┐    ┌──────────────────┐
│  SqlBackendBase   │  │   JsonFileDb      │    │                  │
│  （抽象基类）      │  │   + FileLock      │    │   jsonfile 独立  │
│  语句构造/genSql  │  │   文件存储，       │    │   于 SQL 技术栈  │
│  事务循环，仅一份  │  │   独立代码路径     │    │                  │
└─────────┬─────────┘  └───────────────────┘    └──────────────────┘
          │ 虚拟方言钩子
          │ + IDbConnection（驱动执行面）
   ┌──────┼──────────┬──────────────┐
   ▼      ▼          ▼              ▼
 sqlite3  mysql   postgres        dm8
```

- **`SqlBackendBase`**（`src/base/SqlBackendBase.h/.cpp`）是抽象基类，持有 8 个 `Idb` 方法骨架、语句构造器、智能查询装配（`genSql`）、分页统计与事务循环——只编译一次。各后端通过 **`IDbConnection`** 接口驱动底层驱动库，只重写与默认实现不同的**虚方言钩子**。
- **`DbPool`**（`src/base/DbPool.h`）是共享连接池：按 `db_conn` 懒创建连接、每条语句独占 RAII `Lease`（`select` 的主查询与 records 统计跑在同一连接上）、池耗尽时阻塞等待、连接致命错误后 `invalidate()` 自愈。
- **jsonfile**（`src/backends/JsonFileDb.h/.cpp` + `FileLock.h/.cpp`）独立于 SQL 技术栈：单个 JSON 文件 + 跨进程文件锁 + 原子写入 + 损坏文件自动备份 + 按 id 的 O(1) 索引。

### 方言钩子——谁重写了什么

相似的后端天然共享默认实现，只有真正的独有差异才需要重写：

| 钩子（默认实现在 `SqlBackendBase.cpp`） | sqlite3 | mysql | postgres | dm8 |
|---|---|---|---|---|
| 占位符 `?` / `limit o,n` / 标识符不引号 / 逗号拼接投影 | 默认 | 默认 | 重写（$n、`limit N OFFSET o`） | 重写（"引号小写"） |
| upsert `ON CONFLICT ... excluded` | 默认 | 重写（`ON DUPLICATE KEY ... values()`） | 默认 | —（create() 内读后写） |
| 字面量转义（单引号翻倍） | 默认 | 重写（`mysql_real_escape_string`） | 默认 | 默认 |
| LIKE / fuzzy 列 | 默认 | 默认 | 重写（`CAST(col as TEXT)`） | 重写（加引号） |
| **需要重写数** | **0** | **2** | **5** | **9 + create/insertBatch** |

sqlite3 只剩纯驱动代码——这就是去重程度的标尺。新增一个后端 = 写一个 `Connection` 类 + 只重写自己方言写法不同的钩子。

## 项目结构

```
zorm/
├── CMakeLists.txt            # zormlib 静态库 + 演示 exe + 测试目标
├── CMakePresets.json         # clang-dbg / clang-rlz（Ninja + MSYS2 clang64）
├── run-test                  # 测试运行器（见"单元测试"）
├── src/
│   ├── main.cpp              # 演示入口（链接 zormlib）
│   ├── DbBase.cpp            # 工厂实现——唯一能看到后端头文件的编译单元
│   ├── GlobalConstants.cpp   # 状态码消息表
│   ├── include/              # ★ 对外公共接口（仅 4 个头，保持最小）
│   │   ├── Idb.h             # 统一接口（签名冻结）
│   │   ├── DbBase.h          # 工厂声明（不泄漏任何驱动头文件）
│   │   ├── GlobalConstants.h # 状态码（200/202/301/701/...）
│   │   └── dll_global.h      # ZORM_API 宏
│   ├── base/                 # 私有共享算法层
│   │   ├── SqlBackendBase.h/.cpp  # Idb 骨架、语句构造、genSql、事务循环
│   │   ├── DbConnection.h    # IDbConnection：单连接执行面
│   │   ├── DbPool.h          # HandlePool + 独占 RAII 租约（模板）
│   │   └── DbUtils.h/.cpp    # SQL/JSON 辅助、GenerateId、Trim
│   └── backends/             # 私有的各后端 .h/.cpp 对
│       ├── Sqlit3Db.h/.cpp   # sqlite3（零方言重写——纯驱动）
│       ├── MysqlDb.h/.cpp    # mysql（upsert/转义重写、TLS 选项）
│       ├── PostgresDb.h/.cpp # postgres（$n 占位、CAST LIKE、OFFSET）
│       ├── Dm8Db.h/.cpp      # dm8（引号小写组、读后写 upsert）
│       ├── JsonFileDb.h/.cpp # jsonfile 后端
│       ├── FileLock.h/.cpp   # JsonFileDb 使用的跨进程锁
│       └── pg_type_d.h       # libpq OID 表（私有）
├── tests/                    # 见"单元测试"
├── docs/                     # project-index、jsonfile-design、代码评审记录
└── thirds/                   # 捆绑依赖：googletest、sqlite3、mysql、pq、dm8、zjson
```

## 数据库通用接口
  > 应用类直接操作这个通用接口，实现与底层实现数据库的分离。该接口提供了CURD标准访问，以及批量插入和事务操作，基本能满足平时百分之九十以上的数据库操作。

  ```
    class ZORM_API Idb
    {
    public:
        virtual Json select(const string& tablename, const Json& params, vector<string> fields = vector<string>(), Json values = Json(JsonType::Array)) = 0;
        virtual Json create(const string& tablename, const Json& params) = 0;
        virtual Json update(const string& tablename, const Json& params) = 0;
        virtual Json remove(const string& tablename, const Json& params) = 0;
        virtual Json querySql(const string& sql, Json params = Json(), Json values = Json(JsonType::Array), vector<string> fields = vector<string>()) = 0;
        virtual Json execSql(const string& sql, Json params = Json(), Json values = Json(JsonType::Array)) = 0;
        virtual Json insertBatch(const string& tablename, const Json& elements, string constraint = "id") = 0;
        virtual Json transGo(const Json& sqls, bool isAsync = false) = 0;
    };
  ```

每个方法都返回带 `status` 字段的 Json 对象。主要状态码（`GlobalConstants.h`）：

| 码值 | 含义 |
|------|------|
| 200 | 成功 |
| 202 | 查询结果为空 |
| 301 | 参数错误（形态不对 / 缺 id / 保留字畸形） |
| 700 | 数据库连接失败 |
| 701 | 数据库操作失败（附带驱动错误消息） |
| 404 / 500 / 702 / 703 / 801 | 见 `src/include/GlobalConstants.h` |

### 行为契约（所有后端一致）

| 调用 | 语义 |
|------|------|
| `create(table, params)` | 对象 → 插入；id 缺失/为空时自动生成 8 位十六进制 id；提供的 id 已存在时**覆盖写入（upsert）**；返回 `{status, id, insertId, affectedRows}` |
| `create(table, array)` | 1 个元素 → 走单行路径；N 个元素 → 转调 `insertBatch` |
| `update(table, params)` | 必须带 `id` 且至少一个要更新的列（否则 301）；返回 `affectedRows` |
| `remove(table, params)` | 必须带 `id`（否则 301） |
| `select(table, params, fields)` | 智能查询（见下）；除 `data` 外总是返回 `records` + `pages` |
| `querySql(sql, params, values, fields)` | 原生查询；`params` 承载智能查询保留字，`values` 绑定到 `?`/`$n` 占位符 |
| `execSql(sql, params, values)` | 原生语句；返回 `affectedRows` |
| `insertBatch(table, elements, constraint)` | 一条多行 INSERT；主键重复则更新（upsert）；接受单个元素 |
| `transGo(sqls, isAsync)` | 事务，见下 |

### transGo 元素格式

事务是一个元素数组；每个元素既可以是原生 SQL 文本，也可以是结构化操作：

```
// 原生 SQL，可带占位符
{ "text": "insert into users (id,name) values (?,?)", "values": ["a1b2c3d4", "john"] }

// 结构化操作（SQL 按各方言生成，标识符正确加引号）
{ "table": "users", "method": "Insert", "params": {"id": "...", "name": "john"} }
{ "table": "users", "method": "Update", "id": "...", "params": {"name": "jane"} }
{ "table": "users", "method": "Delete", "id": "..." }
{ "table": "users", "method": "Batch",  "params": [ {...}, {...} ] }
```

任一语句失败即整体回滚，失败状态中附带驱动错误消息。

## 实例构造
> 全局查询开关变量：
- DbLogClose : sql 查询语句显示开关
- parameterized : 是否使用参数化查询

> Sqlite3:
```
    Json options;
    options.add("connString", "./db.db");    //数据库位置
    options.add("DbLogClose", false);        //显示查询语句
    options.add("parameterized", false);     //不使用参数化查询
    DbBase* db = new DbBase("sqlite3", options);
```
  
> Mysql:
```
    Json options;
    options.add("db_host", "192.168.6.6");   //mysql服务IP
    // SSL/TLS（可选）：服务器开启SSL时客户端自动协商TLS；以下选项按需配置
    //options.add("db_ssl_ca", "./ca.pem");        //CA证书，用于校验服务器证书
    //options.add("db_ssl_cert", "./client.pem");  //客户端证书（双向认证）
    //options.add("db_ssl_key", "./client.key");   //客户端私钥
    //options.add("db_ssl_verify", true);          //校验服务器证书（默认false，自签名证书环境可不开）
    //options.add("db_ssl_required", true);        //强制TLS：服务器不支持加密连接时直接失败
    options.add("db_port", 3306);            //端口
    options.add("db_name", "dbtest");        //数据库名称
    options.add("db_user", "root");          //登记用户名
    options.add("db_pass", "123456");        //密码
    options.add("db_char", "utf8mb4");       //连接字符设定[可选]
    options.add("db_conn", 5);               //连接池配置[可选]，默认为2
    options.add("DbLogClose", true);         //不显示查询语句
    options.add("parameterized", true);      //使用参数化查询
    DbBase* db = new DbBase("mysql", options);
```

> Postgres:
```
    Json options;
    options.add("db_host", "192.168.6.6");
    options.add("db_port", 5432);
    options.add("db_name", "dbtest");
    options.add("db_user", "root");
    options.add("db_pass", "123456");
    options.add("db_conn", 5);
    options.add("DbLogClose", false);
    options.add("parameterized", true);
    DbBase* db = new DbBase("postgres", options);
```

> Dm8:
```
    Json options;
    options.add("db_host", "192.168.5.12");  //达梦服务IP
    options.add("db_port", 5236);            //端口，默认 5236
    options.add("db_name", "dbtest");        //模式名
    options.add("db_user", "SYSDBA");        //用户名
    options.add("db_pass", "123456");        //密码
    options.add("db_char", "utf8mb4");       //连接字符设定[可选]
    options.add("db_conn", 1);               //连接池配置[可选]，默认为1
    options.add("DbLogClose", false);
    options.add("parameterized", true);
    DbBase* db = new DbBase("dm8", options);
```
> 达梦注意：zorm 生成的 SQL 中标识符一律带小写引号；自己写原生 SQL 时也请保持引号（CASE_SENSITIVE=Y 的服务端会把不带引号的标识符折叠为大写）。

> JsonFile:
```
    Json options;
    options.add("connString", "./data.json");  //数据文件位置，留空则为"程序目录/data.json"
    options.add("DbLogClose", true);           //不显示查询语句
    DbBase* db = new DbBase("jsonfile", options);
```
  > 数据以JSON数组形式存储在单个文件中：`[{"table":"t1","columns":["id",...],"rows":[{...},...]}]`。
  > 支持全部通用接口（CRUD、批量插入、事务、智能查询），内建跨进程文件锁、原子写入、
  > 损坏文件自动备份与按 id 的 O(1) 索引；也可直接使用 `JsonFileDb` 类
  > （`ZORM::JsonFile::JsonFileDb`，提供 `createShared` 工厂方法）。
  > 提示：值若是一段"看起来像 JSON 的文本"（如 `[1,2]`、`{"a":1}`），请用
  > `Json::str(text)` 构造——`Json(text)` 会把它按文档解析成数组/对象。

## 智能查询方式设计
> 查询保留字：page, size, sort, fuzzy, lks, ins, ors, count, sum, group

- page, size, sort, 分页排序
    在sqlit3与mysql中这比较好实现，limit来分页是很方便的，排序只需将参数直接拼接到order by后就好了。  
    查询示例：
    ```
    Json p;
    p.add("page", 1);
    p.add("size", 10);
    p.add("sort", "age desc");
    (new DbBase(...))->select("users", p);
    
    生成sql：   SELECT * FROM users  ORDER BY age desc LIMIT 0,10
    ```
- fuzzy, 模糊查询切换参数，不提供时为精确匹配
    提供字段查询的精确匹配与模糊匹配的切换。
    ```
    Json p;
    p.add("username", "john");
    p.add("password", "123");
    p.add("fuzzy", 1);
    (new DbBase(...))->select("users", p);
   
    生成sql：   SELECT * FROM users  WHERE username like '%john%'  and password like '%123%'
    ```
- ins, lks, ors
    这是最重要的三种查询方式，如何找出它们之间的共同点，减少冗余代码是关键。

    - ins, 数据库表单字段in查询，一字段对多个值，例：  
        查询示例：
    ```
    Json p;
    p.add("ins", "age,11,22,36");
    (new DbBase(...))->select("users", p);

    生成sql：   SELECT * FROM users  WHERE age in ( 11,22,26 )
    ```
    - ors, 数据库表多字段精确查询，or连接，多个字段对多个值，例：  
        查询示例：
    ```
    Json p;
    p.add("ors", "age,11,age,36");
    (new DbBase(...))->select("users", p);

    生成sql：   SELECT * FROM users  WHERE  ( age = 11  or age = 26 )
    ```
    - lks, 数据库表多字段模糊查询，or连接，多个字段对多个值，例：
        查询示例：
    ```
    Json p;
    p.add("lks", "username,john,password,123");
    (new DbBase(...))->select("users", p);

    生成sql：   SELECT * FROM users  WHERE  ( username like '%john%'  or password like '%123%'  )
    ```
- count, sum
    这两个统计求和，处理方式也类似，查询时一般要配合group与fields使用。
    - count, 数据库查询函数count，行统计，例：
        查询示例：
    ```
    Json p;
    p.add("count", "1,total");
    (new DbBase(...))->select("users", p);

    生成sql：   SELECT *,count(1) as total  FROM users
    ```
    - sum, 数据库查询函数sum，字段求和，例：
        查询示例：
    ```
    Json p;
    p.add("sum", "age,ageSum");
    (new DbBase(...))->select("users", p);

    生成sql：   SELECT username,sum(age) as ageSum  FROM users
    ```
- group, 数据库分组函数group，例：  
    查询示例：
    ```
    Json p;
    p.add("group", "age");
    (new DbBase(...))->select("users", p);

    生成sql：   SELECT * FROM users  GROUP BY age
    ```

> 不等操作符查询支持

支持的不等操作符有：>, >=, <, <=, <>, =；逗号符为分隔符，一个字段支持一或二个操作。  
特殊处：使用"="可以使某个字段跳过search影响，让模糊匹配与精确匹配同时出现在一个查询语句中

- 一个字段一个操作，示例：
    查询示例：
    ```
    Json p;
    p.add("age", ">,10");
    (new DbBase(...))->select("users", p);

    生成sql：   SELECT * FROM users  WHERE age> 10
    ```
- 一个字段二个操作，示例：
    查询示例：
    ```
    Json p;
    p.add("age", ">=,10,<=,33");
    (new DbBase(...))->select("users", p);

    生成sql：   SELECT * FROM users  WHERE age>= 10 and age<= 33
    ```
- 使用"="去除字段的fuzzy影响，示例：
    查询示例：
    ```
    Json p;
    p.add("age", "=,18");
    p.add("username", "john");
    p.add("fuzzy", "1");
    (new DbBase(...))->select("users", p);

    生成sql：   SELECT * FROM users  WHERE age= 18  and username like '%john%'
    ```

> 分页响应形态：设置了 `page`/`size` 后，每次 `select` 都额外返回 `records`
> （总行数，与主查询在同一连接上统计）和 `pages`（总页数）；未分页时 `records`
> 即返回行数。保留字畸形（如 `ins` 缺值、元素个数为奇数、比较操作符带三个
> 分量等）在所有后端一律返回 301。

 具体使用方法，请参看uint test。 

## 单元测试
采用配置驱动的契约测试套件（gels 风格）：一套测试，六个后端。被测后端由
`tests/dbconfig.json` 中的 `db_dialect` 或 `--dialect` 参数决定——切换被测
数据库就是切换一个配置值，与 refer/gels 完全一致。

同一套测试覆盖的后端：
- `sqlite3-mem` — 内存型 SQLite（无需服务器）
- `sqlite3` — 文件型 SQLite（无需服务器）
- `jsonfile` — JSON 文件型后端（无需服务器）
- `mysql`、`postgres`、`dm8` — 远程服务器（见 dbconfig.json）

共享断言覆盖各后端行为的**交集**——Idb 全部 8 个方法、全部保留字、类型保真
（decimal/datetime 精确往返、聚合精度、NULL 渲染）与转义保真（引号/百分号/
"像 JSON 的文本"在参数化与字面量两种模式下都精确往返）——以及所有后端共享
的参数错误路径（301 形态）。

运行方式（13 个 CTest 注册）：
```
./run-test                 # 默认: sqlitemem + jsonfile 加固 + dbutils + pool
./run-test local           # sqlitemem + sqlite(文件) + json(文件) + 加固 + 单元测试
./run-test remote          # mysql + postgres + dm8
./run-test all             # 全部——回归门
./run-test sqlitemem       # 仅内存型 sqlite
./run-test sqlite          # 仅文件型 sqlite
./run-test json            # jsonfile 契约 + 加固（两者一起跑）
./run-test utils           # DbUtils + DbBase 门面单元测试（完全不碰数据库）
./run-test pool            # HandlePool 租约/阻塞/自愈单元测试（完全不碰数据库）
./run-test mysqlplain      # mysql, parameterized=false（覆盖字面量 SQL 路径）
./run-test sqliteplain     # sqlitemem, parameterized=false（同上）
./run-test pgplain         # postgres, parameterized=false（同上）
./run-test dmplain         # dm8, parameterized=false（同上）
```
每个 SQL 方言都注册了两个 CTest 用例（默认 + `*plain`）：字面量 SQL 生成、
转义与非参数化解码是独立代码路径。jsonfile 后端另有独立的存储引擎加固测试
（损坏文件备份、跨进程锁、原子写、内存与磁盘一致性、UTF-8 校验），
`./run-test json` 会把它与共享契约套件一起运行。与后端无关的单元
（DbUtils 辅助函数、DbBase 门面校验、连接池）有自己的离线单元测试——
全程不涉及数据库。
> 详见 [docs/jsonfile-design.md](docs/jsonfile-design.md)：设计思想与测试详解；
> 契约测试与解码守门详见 [docs/code-review-contract-suite.md](docs/code-review-contract-suite.md)。
> 测试用例运行结果样例
![输入图片说明](tests/uniTest.PNG)

## 项目地址
```
https://gitee.com/zhoutk/zorm
或
https://github.com/zhoutk/zorm
```

## 运行方法
该项目在vs2022, gcc12.12.0(最低gcc8.5.0), clang12.0下均编译运行正常。
```
git clone https://github.com/zhoutk/zorm
cd zorm
cmake -Bbuild .

---windows
cd build && cmake --build .

---linux & macos
cd build && make

run zorm or ctest
```
构建产物输出到 `bin/`：演示程序 `zorm`（开启测试时还有各测试可执行文件）。
在 MSYS2/clang64 下可用预设更快构建：`cmake --preset clang-dbg` 后
`cmake --build build`；`./run-test` 会为测试自动设置 DLL 搜索路径。

在自己的项目中使用 zorm：用 `add_subdirectory` 引入本仓库（或已安装的副本），
然后链接 `zormlib` 并把 `src/include` 加入头文件搜索路径——这就是全部对外接口。

- 注1：在linux下需要先行安装mysql开发库, 并先手动建立数据库 dbtest。  
在ubuntu下的命令是： apt install libmysqlclient-dev  
- 注2：在linux下需要先行安装 libpq 开发库（要求gcc版本高于8）。  
在ubuntu下的命令是： apt-get install libpq-dev  
- 注3：在macos下需要先行安装 postgresql@14 开发库。  
命令是： brew install postgresql@14  
- 注4：在windows下，若想使用pqxx分支，需编译libpqxx7.7.4，命令如下：
cmake -A x64 -DBUILD_SHARED_LIBS=on -DSKIP_BUILD_TEST=on -DPostgreSQL_ROOT=/d/softs/pgsql ..
cmake --build . --config Release
cmake --install . --prefix /d/softs/libpqxx  
- 注5：在windows下，postgres10是支持win32的最后一个版本，我先择只支行64位版本，选择了最高版本。
- 注6：关于pqxx分支，在windows下, postgres 只能链接 libpqxx7.7.4 的 debug 版， 运行时会出错， Expression:__acrt_first_block==header， 我正在努力解决 ...

## 相关项目

会有一系列项目出炉，网络服务相关，敬请期待...

[gitee-Zjson](https://gitee.com/zhoutk/zjson.git) 
[github-Zjson](https://github.com/zhoutk/zjson.git)
