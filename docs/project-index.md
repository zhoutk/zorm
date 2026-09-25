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
| `jsonfile` | JSON file | pure C++17 file backend (JsonFileDb.cpp) |
| `mysql` | SQL | MariaDB Connector/C on MINGW |
| `postgres` | SQL | libpq |
| `dm8` | SQL | DM8 (达梦) DPI |

The interface, status codes and response shapes are shared across all backends,
so switching databases at runtime is a single config change (see tests).

Consumers include only `src/include/DbBase.h` (or `Idb.h`) and link the
`zormlib` static library - driver headers (mysql.h, DPI.h, ...) never leak
into client code. The whole stack is compiled once in the library; the demo
executable and every test binary link it instead of recompiling sources.

Each SQL dialect is registered twice: once with bound parameters (the default)
and once with `parameterized=false` (`sqliteplain` / `mysqlplain` / `pgplain` /
`dmplain`), because literal-SQL generation, escaping and non-parameterized
decoding are separate code paths. 13 CTest registrations total (6 dialect
contracts + 4 plain variants + jsonfile hardening + dbutils + pool); `./run-test all`
runs them all.

---

## 2. Repository layout

```
zorm/
├── CMakeLists.txt            # zormlib static lib + demo exe + test targets
├── CMakePresets.json
├── version                   # VERSION_MAJOR/MINOR/PATCH
├── run-test                  # test runner: sqlitemem|sqlite|json|mysql|pg|dm|
│                             #   sqliteplain|mysqlplain|pgplain|dmplain|
│                             #   utils|pool|local|remote|all
├── src/
│   ├── main.cpp              # demo entry (links zormlib)
│   ├── DbBase.cpp            # factory impl - the ONLY TU that sees backend headers
│   ├── GlobalConstants.cpp   # status-code message table
│   ├── include/              # ★ PUBLIC surface (4 headers, keep minimal)
│   │   ├── Idb.h             # THE unified interface (keep unchanged)
│   │   ├── DbBase.h          # factory declaration (no driver headers leak)
│   │   ├── GlobalConstants.h # status codes (200/202/301/701/...)
│   │   └── dll_global.h      # ZORM_API macro
│   ├── base/                 # private shared algorithm layer
│   │   ├── SqlBackendBase.h/.cpp  # ★ Idb skeletons, builders, genSql, tx loop
│   │   ├── DbConnection.h    # IDbConnection: per-connection execution surface
│   │   ├── DbPool.h          # HandlePool + exclusive RAII leases (template)
│   │   └── DbUtils.h/.cpp    # SQL/JSON helpers, GenerateId, Trim
│   └── backends/             # private per-backend pairs
│       ├── Sqlit3Db.h/.cpp   # sqlite3 (zero dialect overrides - pure driver)
│       ├── MysqlDb.h/.cpp    # mysql (upsert/escaping overrides, TLS options)
│       ├── PostgresDb.h/.cpp # postgres ($n placeholders, CAST LIKE, OFFSET)
│       ├── Dm8Db.h/.cpp      # dm8 (quoted-lowercase group, read-then-write upsert)
│       ├── JsonFileDb.h/.cpp # jsonfile backend
│       ├── FileLock.h/.cpp   # cross-process lock used by JsonFileDb
│       └── pg_type_d.h       # libpq OID table (private)
├── tests/
│   ├── dbconfig.json         # ★ config: db_dialect + per-backend options/DDL/hooks
│   ├── TestConfig.h/.cpp     # loads dbconfig.json, resolves --dialect
│   ├── ContractSuite.h       # ★ ONE shared contract suite (gels-style)
│   ├── test_contract.cpp     # single test binary; --dialect selects backend,
│   │                         #   --no-param runs the literal-SQL (non-bound) paths
│   ├── test_jsonfile.cpp     # jsonfile file-hardening suite (corrupt/lock/atomic/...)
│   ├── test_dbutils.cpp      # DbUtils + DbBase facade validation (offline)
│   └── test_pool.cpp         # HandlePool lease/blocking/invalidate tests (offline)
├── docs/
│   ├── project-index.md      # ★ this index
│   ├── jsonfile-design.md    # ★ JsonFileDb design & hardening-test details
│   ├── mcp/                  # MCP protocol text (2026-07-28), demo design,
│   │                         #   migration plan, mcp-cpp review
│   └── code-review-contract-suite.md  # ★ review findings, O-1..O-11 fixes,
│                              #   the shared-layer refactor and the test
│                              #   hardening + mutation-verification record
├── thirds/                   # bundled deps: googletest, sqlite3, mysql, pq,
│   │                         #   dm8, zjson, httplib
│   └── mcp-cpp/              # ★ our own C++ MCP SDK (protocol-faithful,
│                             #   do not bend it for app needs)
├── demo/                     # MCP showcase (validation): zorm_analytics_server,
│                             #   demo_data seeding, run-demo scripts, publish
│                             #   pack (demo/zorm-mcp/), README_demo.md
├── zorm-mcp/                 # ★ PRODUCTION MCP server (see section 8)
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
  `PlaceholderSql` / `TypeFidelity` / `EscapingFidelity`.
- `tests/test_jsonfile.cpp` keeps the file-hardening suite (corrupt-file
  backup, cross-process lock, atomic write, persistence, shared instance,
  UTF-8 encoding) — features unique to the file backend.
  → **详见 [jsonfile-design.md](jsonfile-design.md)**：JsonFileDb 的设计思想、
  与 SQL 后端的本质区别、全部 16 组加固测试的逐条说明。
- Old per-database test files (`test_sqlite3.cpp`, `test_mysql.cpp`, ...) were
  **removed** — they duplicated the suite and hard-coded connections.

### Run

```bash
cmake --preset clang-dbg              # configure (Ninja + clang64)
cmake --build build -j 8              # build (zormlib + exes + tests)
./run-test                            # sqlitemem + jsonfile hardening + dbutils + pool
./run-test local                      # all no-server backends + unit tests
./run-test remote                     # mysql + postgres + dm8 (need servers)
./run-test all                        # everything (13 registrations)
./run-test sqlitemem | sqlite | json | mysql | pg | dm
./run-test utils | pool               # offline unit tests (no db)
```

CTest registrations: `test_contract_sqlite3_mem`, `test_contract_sqlite3`,
`test_contract_jsonfile`, `test_contract_mysql`, `test_contract_postgres`,
`test_contract_dm8`, their 4 `*_plain` variants, `test_jsonfile`,
`test_dbutils`, `test_pool`.

### To add a SQL backend

1. Write `src/backends/<Name>Db.h/.cpp`: subclass `SqlBackendBase`, define a
   nested `Connection : IDbConnection` around the native handle, implement
   `acquireConnection()` (pool + connect), and override ONLY the dialect
   hooks that differ from the defaults (see the hook table in
   `src/base/SqlBackendBase.h`). sqlite3 needs zero overrides; postgres
   overrides 5; that is the calibration point.
2. Add the `.cpp` to `ZORM_LIB_SOURCES` in CMakeLists and the include/link
   paths of the new client library.
3. Add a `backends.<name>` block in `tests/dbconfig.json` (options + DDL +
   hooks) and a `ZORM_CONTRACT_TESTS()` instantiation in `test_contract.cpp`
   if the dialect needs its own Env.
4. Register the test (and its `--no-param` variant) in CMakeLists + run-test.
5. Run `./run-test all` — the whole suite now covers it.

---

## 5. zorm-mcp — the production MCP server (`zorm-mcp/`)

The product layer built on the frozen `Idb` surface + `thirds/mcp-cpp`
(branch `mcp`; the validation showcase lives in `demo/`).

- **Binary**: `zorm_mcp_server` (stdio by default, `--http N` for Streamable
  HTTP, `--config F` for backends, `--yes` / `--allow-sql` switches).
- **13 tools, all taking a `db` routing param**: metadata (`db_list_tables`,
  `db_describe_table`, `db_indexes`, `db_info`), query (`db_query` smart
  query, `db_count`, `db_query_sql` read-only gated), writes (`db_insert`,
  `db_insert_batch`, `db_update`, `db_delete`, `db_trans` — ALL behind the
  MRTR `input_required`/`requestState` confirmation), `db_exec_sql`
  (hidden unless `--allow-sql`).
- **Full Idb coverage**: select→db_query, create→db_insert,
  insertBatch→db_insert_batch, update/remove→db_update/db_delete,
  querySql→db_query_sql, execSql→db_exec_sql, transGo→db_trans.
- **Schema metadata** (`db_describe_table`, the "get table fields even when
  EMPTY" capability) comes from per-dialect catalogs: sqlite pragma **TVFs**
  (bare `pragma` breaks the backend's ` limit 1` column probe; `notnull` is
  a reserved word — quote aliases), information_schema (mysql/postgres),
  user_tab_columns (dm8), the jsonfile columns array + row-key fallback.
- **Extras**: `db://schema` + `db://guide` resources, `db-playbook` prompt,
  completion (db/table/fields), self-correcting error messages, the
  four-layer AI-comprehension design (instructions → tool descriptions →
  error teaching → pull channels).
- **Code map**: `src/app.*` (state + catalog routing), `src/guide.*`
  (AI-comprehension texts), `src/meta_tools.cpp` / `src/data_tools.cpp` /
  `src/sql_tools.cpp` (tools + MRTR machinery), `src/resources.cpp`,
  `src/main.cpp`.
- **Tests**: `test_zorm_mcp.cpp` e2e (real stdio child) — 7 cases covering
  metadata-on-empty-table, the full data plane, jsonfile parity, MRTR
  approve/reject/flat-echo/stale-state, trans + exec_sql confirmation,
  graceful shutdown. CTest: `mcp_test_zorm_mcp`.
- **Docs**: `zorm-mcp/README.md` (product overview) ·
  `zorm-mcp/INTEGRATION.md` (per-client setup: Claude Code/Desktop, dsh,
  VS Code/Copilot, Cursor, Windsurf, Cline/Roo, Codex, Gemini CLI,
  Inspector, generic HTTP; placeholders for pi/Reasonix/Grok build).

---

## 6. Reference projects (`refer/`, NOT part of this repo)

- `refer/gels` — TypeScript. `src/config/configs.ts` is the config-driven
  pattern we copied; `src/db/*` (jsonFileDao, sqlite3Dao, mysqlDao,
  postgresDao) share `sqlDialect.ts`/`sqlQueryBuilder.ts`; `test/support/
  rsContract.ts` is the shared contract suite.
- `refer/orm` — C++ orm. `tests/orm_contract_tests.cpp` covers JsonFileDb
  hardening (lock, corrupt file, memory contract, encoding).

---

## 7. Conventions / gotchas

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

## 8. Status codes quick reference

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
