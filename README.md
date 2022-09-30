# zrom  &emsp;&emsp;  [中文介绍](https://github.com/zhoutk/zorm/blob/master/README_CN.md)  

## Introduce  
The basic models of ORM use to be separated from the database. Almost all of them build models at the level of programming language, and let the program deal with all things of the database. Although it is separated from the specific operation of the database, we have to establish various models and write the relationship between tables etc. This is very unfriendly to ordinary developers. My idea is to design tables use tools of relational databases, in our project, json objects can be automatically mapped into standard SQL. As long as we understand the standard SQL language, we can complete the database query operation. Furthermore, We can handled the relationship between tables through views or stored procedures. So our appliction can process all things only using Zorm and Json.

## Related items
This project relies on my other project Zjson, which provides a simple, convenient and efficient Json library. The library is easy to use, a single file library, you only need to download and import the project. Please move to [gitee-Zjson](https://gitee.com/zhoutk/zjson.git) 或 [github-Zjson](https://github.com/zhoutk/zjson.git).

## Design ideas
ZORM data transmission using json, so that data style can be unified from the front to the end. This project aims to be used not only in C++, but also as a dynamic link library used by node.js etc. So we hope to operate json concisely and conveniently like javascript. Therefore, the zjson library was established before this. The general operation interface of database is designed separating from the databases. This interface provides CURD standard api, as well as batch insert and transaction operations, which can basically cover more than 90% of normal database operations. The basic goal of the project is to support Sqlite 3, MySQL, and Postges. Can running on Windows, Linux, or MacOS.

## Project progress
Now all functions of using sqlit3 and mysql have been implemented. Postgres's has completed on linux and macos. The technologies I used is sqlit3 - sqllit3.h（c api）；mysql - c api （MySQL Connector C 6.1）；postgres - libpqxx7.7.4 。

任务列表：
- [x] Sqlite3
  - [x] linux 
  - [x] windows
  - [x] macos
- [x] Mysql
  - [x] linux 
  - [x] windows
  - [x] macos
- [x] Postgre
  - [x] linux 
  - [ ] windows
  - [x] macos

## Database interface
  > The interface was designed to separate operations from databases. 

  ```
    class ZORM_API Idb
    {
    public:
        virtual Json select(string tablename, Json& params, vector<string> fields = vector<string>(), Json values = Json(JsonType::Array)) = 0;
        virtual Json create(string tablename, Json& params) = 0;
        virtual Json update(string tablename, Json& params) = 0;
        virtual Json remove(string tablename, Json& params) = 0;
        virtual Json querySql(string sql, Json params = Json(), Json values = Json(JsonType::Array), vector<string> fields = vector<string>()) = 0;
        virtual Json execSql(string sql, Json params = Json(), Json values = Json(JsonType::Array)) = 0;
        virtual Json insertBatch(string tablename, Json& elements, string constraint = "id") = 0;
        virtual Json transGo(Json& sqls, bool isAsync = false) = 0;
    };
  ```

## Example of DbBase
> Global query switch variable:
- DbLogClose : show sql or not
- parameterized : query using parameterized or not

> Sqlite3:
```
    Json options;
    options.addSubitem("connString", "./db.db");    //where database locate
    options.addSubitem("DbLogClose", false);        //show sql
    options.addSubitem("parameterized", false);     //no parameterized
    DbBase* db = new DbBase("sqlite3", options);
```
  
> Mysql:
```
    Json options;
    options.addSubitem("db_host", "192.168.6.6");   //mysql service IP
    options.addSubitem("db_port", 3306);            //port
    options.addSubitem("db_name", "dbtest");        //database's name
    options.addSubitem("db_user", "root");          //username
    options.addSubitem("db_pass", "123456");        //password
    options.addSubitem("db_char", "utf8mb4");       //Connection character setting[optional]
    options.addSubitem("db_conn", 5);               //pool setting[optional]，default is 2
    options.addSubitem("DbLogClose", true);         //not show sql
    options.addSubitem("parameterized", true);      //use parameterized
    DbBase* db = new DbBase("mysql", options);
```

> Postgres:
```
    Json options;
    options.addSubitem("db_host", "192.168.6.6");
    options.addSubitem("db_port", 5432);
    options.addSubitem("db_name", "dbtest");
    options.addSubitem("db_user", "root");
    options.addSubitem("db_pass", "123456");
    options.addSubitem("db_conn", 5);
    options.addSubitem("DbLogClose", false);
    options.addSubitem("parameterized", true);
    DbBase* db = new DbBase("postgres", options);
```

## Design of intelligent query use Json
> Query reserved words：page, size, sort, fuzzy, lks, ins, ors, count, sum, group

- page, size, sort &emsp;&emsp;//paging and set query order
    example：
    ```
    Json p;
    p.addSubitem("page", 1);
    p.addSubitem("size", 10);
    p.addSubitem("size", "sort desc");
    (new DbBase(...))->select("users", p);
    
    generate sql：   SELECT * FROM users  ORDER BY age desc LIMIT 0,10
    ```
- fuzzy &emsp;&emsp;//Fuzzy query switch, if not provided, it is exact matching. Provides it or not will switch between exact matching and fuzzy matching.
    ```
    Json p;
    p.addSubitem("username", "john");
    p.addSubitem("password", "123");
    p.addSubitem("fuzzy", 1);
    (new DbBase(...))->select("users", p);
   
    generate sql：   SELECT * FROM users  WHERE username like '%john%'  and password like '%123%'
    ```
- ins, lks, ors &emsp;&emsp;//Three most important query methods. How to find the common points among them is the key to reduce redundant codes.

    - ins &emsp;&emsp;//single field, multiple values：
    ```
    Json p;
    p.addSubitem("ins", "age,11,22,36");
    (new DbBase(...))->select("users", p);

    generate sql：   SELECT * FROM users  WHERE age in ( 11,22,26 )
    ```
    - ors &emsp;&emsp;//exact matching; multiple fields, multiple values：
    ```
    Json p;
    p.addSubitem("ors", "age,11,age,36");
    (new DbBase(...))->select("users", p);

    generate sql：   SELECT * FROM users  WHERE  ( age = 11  or age = 26 )
    ```
    - lks &emsp;&emsp;//fuzzy matching; multiple fields, multiple values：
    ```
    Json p;
    p.addSubitem("lks", "username,john,password,123");
    (new DbBase(...))->select("users", p);

    generate sql：   SELECT * FROM users  WHERE  ( username like '%john%'  or password like '%123%'  )
    ```
- count, sum
    > Two statistics function. 
    - count &emsp;&emsp;//count, line statistics：
    ```
    Json p;
    p.addSubitem("count", "1,total");
    (new DbBase(...))->select("users", p);

    generate sql：   SELECT *,count(1) as total  FROM users
    ```
    - sum &emsp;&emsp;//sum, columns statistics：
    ```
    Json p;
    p.addSubitem("sum", "age,ageSum");
    (new DbBase(...))->select("users", p);

    generate sql：   SELECT username,sum(age) as ageSum  FROM users
    ```
- group &emsp;&emsp;：
    ```
    Json p;
    p.addSubitem("group", "age");
    (new DbBase(...))->select("users", p);

    generate sql：   SELECT * FROM users  GROUP BY age
    ```

> Unequal operator query support

The supported operators are : >, >=, <, <=, <>, = . Comma is the separator. One field supports one or two operations.Special features: using "=" can enable a field to skip the fuzzy matching. So fuzzy matching and exact matching can appear in one query at the same time.

- one field, one operation：
    ```
    Json p;
    p.addSubitem("age", ">,10");
    (new DbBase(...))->select("users", p);

    generate sql：   SELECT * FROM users  WHERE age> 10
    ```
- two field, two operation：
    ```
    Json p;
    p.addSubitem("age", ">=,10,<=,33");
    (new DbBase(...))->select("users", p);

    generate sql：   SELECT * FROM users  WHERE age>= 10 and age<= 33
    ```
- use "=" skip fuzzy matching：
    ```
    Json p;
    p.addSubitem("age", "=,18");
    p.addSubitem("username", "john");
    p.addSubitem("fuzzy", "1");
    (new DbBase(...))->select("users", p);

    generate sql：   SELECT * FROM users  WHERE age= 18  and username like '%john%'
    ```
 Details in unit test, thanks! 

## Unit test
Detailed description, please move to tests catalogue.
> Example of test case running results
![test result](tests/uniTest.PNG)

## Project site
```
https://gitee.com/zhoutk/zorm
or
https://github.com/zhoutk/zorm
```

## run guidance
The project is built in vs2022, gcc12.12.0(at lest gcc8.5.0), clang12.0 success。
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
- note 1：on linux need mysql dev lib and create a db named dbtest first.
the command of ubuntu： apt install libmysqlclient-dev  
- note 2：on linux need libpq dev lib (gcc at least 8).
the command of ubuntu： apt-get install libpq-dev  
- note 3：on macos need libpqxx dev lib.  
the command is ： brew install libpqxx
- note 4：on windows, need postgres database installed and compile libpqxx7.7.4, as follows：
cmake -A x64 -DBUILD_SHARED_LIBS=on -DSKIP_BUILD_TEST=on -DPostgreSQL_ROOT=/d/softs/pgsql ..
cmake --build . --config Release
cmake --install . --prefix /d/softs/libpqxx  
- note 5：on windows, postgres10 is the last version which support win32, So I only support the x64 version using pg14。
- note 6: on windows, postgres can only link libpqxx7.7.4's dll using debug version, and run with a Expression:__acrt_first_block==header, I'm try to solve it ...

## Associated projects

[gitee-Zjson](https://gitee.com/zhoutk/zjson.git) 
[github-Zjson](https://github.com/zhoutk/zjson.git)
