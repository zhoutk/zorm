# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

zorm is a header-based C++17 ORM exposing one interface (`ZORM::Idb`, in `src/include/Idb.h` — treat its signatures as frozen) over five backends: sqlite3, mysql, postgres, dm8 (达梦 DPI), and jsonfile (pure-C++ file storage). Data crosses the API as `Json` (the zjson library, bundled in `thirds/zjson`). `src/include/DbBase.h` is the factory: `new DbBase("sqlite3"|"jsonfile"|..., options)`.

`docs/project-index.md` is a curated repo map; `docs/jsonfile-design.md` documents the file backend.

## Build & test (MSYS2 clang64 / Windows)

```bash
cmake --preset clang-dbg          # or clang-rlz (RelWithDebInfo); Ninja + clang64
cmake --build build               # outputs to bin/
```

Tests are config-driven (gels-style): one contract suite (`tests/ContractSuite.h`), backend selected by `--dialect` / `tests/dbconfig.json`. Run through `./run-test`, which sets up DLL paths (`thirds/bin/win10`) and maps short names to CTest registrations (11 total):

```bash
./run-test                # default: sqlitemem + jsonfile hardening
./run-test local          # sqlitemem + sqlite(file) + json(file)
./run-test remote         # mysql + postgres + dm8 (servers on 10.0.0.7, docker)
./run-test all            # all 11 registrations — the regression gate
./run-test mysqlplain     # parameterized=false variant (also pgplain/dmplain/sqliteplain)
./run-test json -R test_contract_jsonfile -V   # single test, ctest args pass through
```

Known test pitfalls:
- Building (or a coverage build) re-links into `bin/` and can confuse ninja timestamps — if a test run executes stale code, delete the stale exe in `bin/` first.
- A test may segfault once immediately after a build finishes; rerun before investigating.
- Remote tests (mysql/pg/dm) require the docker test servers; plain-name runs and `local`/`json` work offline.

## Architecture

- **SQL backends** (`Sqlit3Db.h`, `MysqlDb.h`, `PostgresDb.h`, `Dm8Db.h`) are thin driver+dialect layers over `src/include/SqlBackendBase.h`, a CRTP base (`SqlBackendBase<Derived, Handle>`) that owns the 8 `Idb` skeletons, SQL builders, `genSql` smart-query assembly (reserved words: page/size/sort/fuzzy/lks/ins/ors/count/sum/group), pagination counters, and the transaction loop. Backends implement ~14 non-virtual CRTP hooks (quoting, placeholder, parameter binding/decoding, escapeString, limit syntax, etc.). Adding a backend = implementing hooks, not copying statements. `DbPool.h` provides `HandlePool<Handle>` + RAII `Lease` (value-semantic handles, blocking wait, `invalidate` self-heal).
- **jsonfile backend** (`JsonFileDb.h` + `src/JsonFileDb.cc`, ~2.5k lines) is independent of the SQL stack: single JSON-file storage with cross-process file lock (`FileLock.h`), atomic writes, corrupt-file backup, O(1) per-table id index.
- **Compiler flags matter**: `-fsigned-char` is set project-wide for GCC/Clang (ARM `char` is unsigned by default) — keep it.
- `refer/` (gels, orm) are read-only reference projects used as design precedents, not part of the build.

## zjson JSON-sniffing convention (important)

`Json(text)` / `add(key, text)` **parse** text beginning with `{` or `[` into objects/arrays. When a *value* is text that merely looks like JSON (`"[1,2]"`, `"{\"a\":1}"`), you must build it with `Json::str(text)` — otherwise type corruption. Bare-string keys/options keep the sniffing semantics by design. History: violations of this in the SQL decode paths and zjson's `extendItem` caused a real bug class (fixed in commit `8ade270`).

## Testing conventions (contract suite)

- `tests/ContractSuite.h` defines one suite (Contract.Read/Write/Query/Dao, TypeFidelity, EscapingFidelity) instantiated per backend via `ZORM_CONTRACT_TESTS()`; each backend test provides an `Env` subclass (connection options + DDL schema + dialect overrides like `rawSelectAll`). Assertions cover the **intersection** of the five backends' behavior; dialect-specific behavior goes in per-backend tests.
- `parameterized=false` (plain) is a separate code path (literal SQL generation + escaping + non-parameterized decode) — every SQL dialect is registered twice. New dialect → also register its `--no-param` variant.
- Adding a column to the contract table → update `tests/dbconfig.json` (all dialect blocks), the embedded DDL in `tests/test_jsonfile.cpp`, and the TypeFidelity assertions, together.
- Regression gates: backend change → `./run-test all`; change to decode/SQL construction → verify with a **mutation** (revert the fix, confirm tests fail, then re-apply — a mutation that doesn't fail means the mutation was ineffective, not that coverage is absent).
- DM8 test server runs CASE_SENSITIVE=Y: unquoted identifiers in raw SQL fold to uppercase; Dm8Db-generated SQL is always quoted-lowercase — keep raw-SQL test hooks quoted.
