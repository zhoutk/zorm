# zorm Project Index

> 给后来的人 / AI 快速了解本项目结构的索引文档。
> Quick-start index for future developers and AI agents.

---

## 1. What is zorm?

A lightweight C++ ORM / database abstraction layer exposing ONE unified
interface (`ZORM::Idb`) across multiple database backends:

| Backend | Type | Notes |
|---------|------|-------|
| `sqlite3-mem` | SQL | in-memory SQLite, no server |
| `sqlite3` | SQL | file-backed SQLite |
| `jsonfile` | JSON file | pure C++17 file backend (JsonFileDb.cc) |
| `mysql` | SQL | MariaDB Connector/C on MINGW |
| `postgres` | SQL | libpq |
| `dm8` | SQL | DM8 (达梦) DPI |

The interface, status codes and response shapes are shared across all backends,
so switching databases at runtime is a single config change (see tests).

Each SQL dialect is registered twice: once with bound parameters (the default)
and once with `parameterized=false` (`sqliteplain` / `mysqlplain`), because the
literal-SQL generation and the non-parameterized decoding are separate code
paths. 9 CTest registrations total; `./run-test all` runs them all.

---

## 2. Repository layout

```
zorm/
├── CMakeLists.txt            # build + test targets (config-driven tests)
├── CMakePresets.json
├── version                   # VERSION_MAJOR/MINOR/PATCH
├── run-test                  # test runner: sqlitemem|sqlite|json|mysql|pg|dm|
│                             #   sqliteplain|mysqlplain|local|remote|all
├── src/
│   ├── main.cc               # demo entry
│   ├── JsonFileDb.cc         # JSON file backend implementation
│   └── include/
│       ├── Idb.h             # THE unified interface (keep unchanged)
│       ├── DbBase.h          # factory: DbBase("sqlite3"|"jsonfile"|...) -> backend
│       ├── DbUtils.h         # SQL/JSON helpers, GenerateId, Trim
│       ├── SqlBackendBase.h  # ★ shared SQL algorithm layer (CRTP dialect base)
│       ├── DbPool.h          # ★ connection pool with exclusive RAII leases
│       ├── GlobalConstants.h # status codes (200/202/301/701/...) + messages
│       ├── Sqlit3Db.h        # sqlite3 backend (driver + dialect only)
│       ├── MysqlDb.h         # mysql backend (driver + dialect + TLS options)
│       ├── PostgresDb.h      # postgres backend (driver + dialect only)
│       ├── Dm8Db.h           # dm8 backend (driver + dialect + upsert overrides)
│       ├── JsonFileDb.h      # jsonfile backend header
│       └── FileLock.h        # cross-process lock used by JsonFileDb
├── tests/
│   ├── dbconfig.json         # ★ config: db_dialect + per-backend options/DDL/hooks
│   ├── TestConfig.h/.cc      # loads dbconfig.json, resolves --dialect
│   ├── ContractSuite.h       # ★ ONE shared contract suite (gels-style)
│   ├── test_contract.cpp     # single test binary; --dialect selects backend,
│   │                         #   --no-param runs the literal-SQL (non-bound) paths
│   └── test_jsonfile.cpp     # jsonfile file-hardening suite (corrupt/lock/atomic/...)
├── docs/
│   ├── project-index.md      # ★ this index
│   ├── jsonfile-design.md    # ★ JsonFileDb design & hardening-test details
│   └── code-review-contract-suite.md  # ★ review findings, O-1..O-11 fixes,
│                              #   the shared-layer refactor and the test
│                              #   hardening + mutation-verification record
├── thirds/                   # bundled deps: googletest, sqlite3, mysql, pq, dm8, zjson
└── refer/                    # reference projects (NOT part of this project)
    ├── gels/                 # TypeScript project - best-in-class DAO layer
    └── orm/                  # C++ orm with JsonFileDb reference impl
```

---

## 3. The unified interface (`Idb.h`)

`ZORM::Idb` (keep unchanged — it is the public contract):

| Method | Signature | Returns |
|--------|-----------|---------|
| `select` | `(table, params, fields, values)` | Json `{status, data, records, pages}` |
| `create` | `(table, params)` | Json `{status, id, insertId, affectedRows}` |
| `update` | `(table, params)` | Json `{status, affectedRows}` |
| `remove` | `(table, params)` | Json `{status, affectedRows}` |
| `querySql` | `(sql, params, values, fields)` | Json `{status, data}` |
| `execSql` | `(sql, params, values)` | Json `{status, affectedRows}` |
| `insertBatch` | `(table, elements, constraint)` | Json `{status, affectedRows}` |
| `transGo` | `(sqls, isAsync)` | Json `{status}` |

Key status codes (`GlobalConstants.h`): `200` success, `202` empty, `301` param
error, `701` db operation failed, `700` connection failed.

**Query params** (select): `fuzzy=1` (LIKE), `ins=col,a,b`, `lks=col,x` (LIKE),
`ors=col,v1,col,v2`, comparison `col =>,21` / `>=,a,<=,b`, `sort=col [asc|desc]`,
`page`/`size`, `group=col`, `sum=col,alias`, `count=*,alias`.

**transGo elements**: SQL text `{text|sql, values}` OR structured
`{table, method: Insert|Update|Delete|Batch, params, id}`.

**Parity guarantees** (all backends behave the same):
- `create()` auto-generates an 8-hex `id` when missing/empty, and **upserts**
  when the id already exists (sqlite3/postgres `ON CONFLICT`, mysql
  `ON DUPLICATE KEY`, dm8 read-then-write, jsonfile native).
- `remove` without `id` → 301.
- `insertBatch` accepts a single element.
- `select` always returns `records` + `pages`.
- NULL reads: sqlite3/postgres/dm8/jsonfile → JSON null; mysql client → "null".

---

## 4. Testing (the heart of this refactor)

**One suite, many backends** — mirrors `refer/gels`:

- `tests/dbconfig.json` defines every backend: connection options, schema DDL,
  dialect hooks (`rawTable`, `quoteColumn`, `placeholder`, `catalogSql`,
  `nullRendering`, `supportsWherePlaceholders`, `autoCreateTables`).
- `tests/test_contract.cpp` is ONE binary; `--dialect <name>` (or
  `ZORM_DB_DIALECT` / `dbconfig.json#db_dialect`) selects the backend.
- `tests/ContractSuite.h` contains the shared bodies:
  `Read` / `Write` / `Query` / `Dao` / `EdgeCases` / `MetadataCatalog` /
  `PlaceholderSql`.
- `tests/test_jsonfile.cpp` keeps the file-hardening suite (corrupt-file
  backup, cross-process lock, atomic write, persistence, shared instance,
  UTF-8 encoding) — features unique to the file backend.
  → **详见 [jsonfile-design.md](jsonfile-design.md)**：JsonFileDb 的设计思想、
  与 SQL 后端的本质区别、全部 16 组加固测试的逐条说明。
- Old per-database test files (`test_sqlite3.cpp`, `test_mysql.cpp`, ...) were
  **removed** — they duplicated the suite and hard-coded connections.

### Run

```bash
cmake -Bbuild .                       # configure
cmake --build build -j 8              # build (test_contract + test_jsonfile)
./run-test                            # sqlitemem + jsonfile hardening
./run-test local                      # all no-server backends
./run-test remote                     # mysql + postgres + dm8 (need servers)
./run-test all                        # everything
./run-test sqlitemem | sqlite | json | mysql | pg | dm
```

CTest registrations: `test_contract_sqlite3_mem`, `test_contract_sqlite3`,
`test_contract_jsonfile`, `test_contract_mysql`, `test_contract_postgres`,
`test_contract_dm8`, `test_jsonfile`.

### To add a backend

1. Implement `ZORM::Idb` (or extend `DbBase`).
2. Add a `backends.<name>` block in `tests/dbconfig.json` (options + DDL +
   hooks).
3. Add one `add_test(... test_contract --dialect <name>)` line in CMakeLists.
4. Run `./run-test <name>` — the whole suite now covers it.

---

## 5. Reference projects (`refer/`, NOT part of this repo)

- `refer/gels` — TypeScript. `src/config/configs.ts` is the config-driven
  pattern we copied; `src/db/*` (jsonFileDao, sqlite3Dao, mysqlDao,
  postgresDao) share `sqlDialect.ts`/`sqlQueryBuilder.ts`; `test/support/
  rsContract.ts` is the shared contract suite.
- `refer/orm` — C++ orm. `tests/orm_contract_tests.cpp` covers JsonFileDb
  hardening (lock, corrupt file, memory contract, encoding).

---

## 6. Conventions / gotchas

- **DM8** folds unquoted identifiers to upper case: always quote identifiers
  (`"dbtest"."table_for_test"`, columns quoted); the count alias is quoted too.
- **Postgres** uses `$n` placeholders; sqlite3/mysql/dm8/jsonfile use `?`.
- **MySQL** client returns NULL as the string `"null"` in its C-API text path.
- `dbconfig.json` is copied next to `test_contract` at build time; CTest runs
  with the exe dir as working directory.
- File-backed test DBs (`zorm_test_sqlite3.db`, `zorm_test_contract.json`) live
  in `bin/` and are DROP/CREATE-cycled by the suite, so they stay clean.
- Build toolchain (msys2 clang64): clang 22, cmake 4.4, ninja 1.13.
  MySQL client = MariaDB Connector/C (`mingw-w64-clang-x86_64-libmariadbclient`).

---

## 7. Status codes quick reference

| Code | Meaning |
|------|---------|
| 200 | success |
| 202 | query result empty |
| 301 | param error |
| 404 | resource not found |
| 500 | exception |
| 700 | db connection failed |
| 701 | db operation failed |
| 702 | table must have id |
| 703 | db modified, restart needed |
| 801 | parent record not found |
