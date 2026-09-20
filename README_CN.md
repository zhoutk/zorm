# zrom  &emsp;&emsp;  [English](README.md)  

## 介绍
我们通用的ORM，基本模式都是想要脱离数据库的，几乎都在编程语言层面建立模型，由程序去与数据库打交道。虽然脱离了数据库的具体操作，但我们要建立各种模型文档，用代码去写表之间的关系等等操作，让初学者一时如坠云雾。我的想法是，将关系数据库拥有的完善设计工具之优势，来实现数据设计以提供结构信息，让json对象自动映射成为标准的SQL查询语句。只要理解了标准的SQL语言，我们就能够完成数据库查询操作。更进一步，可以使用关系数据库的视图和存储过程来处理表之间的关系，并且在应用层使用一个映射（Zrest - next prj will realize），就可以仅仅使用Zorm和Json来完成所有的数据库操作。

## 相关项目
本项目依赖 本人的 另一个项目 Zjson，此项目提供简洁、方便、高效的Json库。该库使用方便，是一个单文件库，只需要下载并引入项目即可。具体信息请移步 [gitee-Zjson](https://gitee.com/zhoutk/zjson.git) 或 [github-Zjson](https://github.com/zhoutk/zjson.git) 。

## 项目名称说明
本人姓名拼音第一个字母z加上orm，即得本项目名称zorm，没有其它任何意义。我将编写一系列以z开头的相关项目，命名是个很麻烦的事，因此采用了这种简单粗暴的方式。

## 设计思路 
ZORM 数据传递采用json来实现，使数据标准能从最前端到最后端达到和谐统一。此项目目标，不但在要C++中使用，还要作为动态链接库与node.js结合用使用，因此希望能像javascript一样，简洁方便的操作json。所以先行建立了zjson库，作为此项目的先行项目。设计了数据库通用操作接口，实现与底层实现数据库的分离。该接口提供了CURD标准访问，以及批量插入和事务操作，基本能满足平时百分之九十以上的数据库操作。项目基本目标，支持Sqlite3,Mysql,Postges,达梦8 四种关系数据库，另含 JsonFile 文件型存储后端，同时支持windows、linux和macOS。

## 项目特点
本系列项目采用单头文件形式开发，使用简单，需要什么，你只要把它下载到你的项目中，include进你的代码，直接使用就好。

## 项目进度
  现在已经实现了基本目标的所有功能。  
  我选择的技术实现方式，基本上是最底层高效的方式。sqlit3 - sqllit3.h（官方的标准c接口）；mysql - c api（MINGW 下用 pacman 的 MariaDB Connector/C，OpenSSL 3 后端支持 TLSv1.2/1.3，MSVC 下仍用第三方目录里的 MySQL Connector C 6.1）；达梦8 - dpi；postgres - c api(pgsql14)；pqxx分支实现了libpqxx7.7.4的封装，linux和macos上运行正常，windows上运行有问题，待解决。
  > 架构说明：四个 SQL 后端共享 `SqlBackendBase.h`（CRTP 方言基座：语句构造、智能查询装配、分页统计、事务循环各只有一份实现）+ `DbPool.h`（RAII 连接租借），后端头文件只保留驱动与方言钩子。

任务列表：
- [x] Sqlite3 实现
  - [x] linux 
  - [x] windows
  - [x] macos
- [x] Mysql 实现
  - [x] linux 
  - [x] windows
  - [x] macos
- [x] Postgre 实现
  - [x] linux 
  - [x] windows
  - [x] macos
- [x] Dm8 实现
  - [x] linux 
  - [x] windows
  - [x] macos
- [x] JsonFile 实现（文件型存储，无需数据库服务器）
  - [x] linux 
  - [x] windows
  - [x] macos

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

## 智能查询方式设计
> 查询保留字：page, size, sort, fuzzy, lks, ins, ors, count, sum, group

- page, size, sort, 分页排序
    在sqlit3与mysql中这比较好实现，limit来分页是很方便的，排序只需将参数直接拼接到order by后就好了。  
    查询示例：
    ```
    Json p;
    p.add("page", 1);
    p.add("size", 10);
    p.add("size", "sort desc");
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

运行方式：
```
./run-test                 # 默认: sqlitemem + jsonfile 加固测试
./run-test local           # sqlitemem + sqlite(文件) + json(文件) + 加固
./run-test remote          # mysql + postgres + dm8
./run-test all             # 全部（9 个测试注册）
./run-test sqlitemem       # 仅内存型 sqlite
./run-test sqlite          # 仅文件型 sqlite
./run-test json            # jsonfile 契约 + 加固（两者一起跑）
./run-test mysqlplain      # mysql, parameterized=false（覆盖字面量 SQL 路径）
./run-test sqliteplain     # sqlitemem, parameterized=false（同上）
./run-test pgplain         # postgres, parameterized=false（同上）
./run-test dmplain         # dm8, parameterized=false（同上）
```
每个被测方言都注册了 CTest 用例（11 个：6 个方言 × 契约 + 4 个 plain 变体 + jsonfile 加固）。
共享契约套件
覆盖 Idb.h 全部 8 个方法的所有参数形态，另外包含：
- **TypeFidelity**：decimal/numeric 与 datetime 列的精确往返、聚合精度、类型列上的
  NULL 渲染——用于守住解码类缺陷（例如 MySQL DECIMAL 在二进制协议中是字符串，
  曾按 double 读出 7e-320 垃圾值）；
- **plain 方言**（4 个）：以 `parameterized=false` 运行同一套契约，覆盖字面量 SQL 生成、
  转义与非参数化解码路径（这些路径与参数化路径是两套代码）；
- **EscapingFidelity**：含引号/百分号/下划线的值必须精确往返（create/等值查询/fuzzy/
  update/批量）——用于守住各方言的字面量转义实现。

jsonfile 后端另有独立的存储引擎加固测试（损坏文件备份、跨进程锁、原子写、
内存与磁盘一致性、UTF-8 校验）。`./run-test json` 会把它与共享契约套件一起运行。
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
