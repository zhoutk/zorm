# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

zorm is a C++17 ORM exposing one interface (`ZORM::Idb`, in `src/include/Idb.h` — treat its signatures as frozen) over five backends: sqlite3, mysql, postgres, dm8 (达梦 DPI), and jsonfile (pure-C++ file storage). Data crosses the API as `Json` (the zjson library, bundled in `thirds/zjson`). `src/include/DbBase.h` + `src/DbBase.cpp` are the factory: `new DbBase("sqlite3"|"jsonfile"|..., options)`.

The library compiles ONCE as the `zormlib` static library (see CMakeLists); executables and tests only link it. `docs/project-index.md` is a curated repo map; `docs/jsonfile-design.md` documents the file backend.

## Build & test (MSYS2 clang64 / Windows)

```bash
cmake --preset clang-dbg          # or clang-rlz (RelWithDebInfo); Ninja + clang64
cmake --build build               # outputs to bin/
```

Tests are config-driven (gels-style): one contract suite (`tests/ContractSuite.h`), backend selected by `--dialect` / `tests/dbconfig.json`. Run through `./run-test`, which sets up DLL paths (`thirds/bin/win10`) and maps short names to CTest registrations (13 total):

```bash
./run-test                # default: sqlitemem + jsonfile hardening + dbutils + pool
./run-test local          # sqlitemem + sqlite(file) + json(file) + dbutils + pool
./run-test remote         # mysql + postgres + dm8 (servers on 10.0.0.7, docker)
./run-test all            # all 13 registrations — the regression gate
./run-test mysqlplain     # parameterized=false variant (also pgplain/dmplain/sqliteplain)
./run-test utils          # DbUtils + DbBase facade unit tests (no db at all)
./run-test pool           # HandlePool lease/invalidate unit tests (no db at all)
./run-test json -R test_contract_jsonfile -V   # single test, ctest args pass through
```

Known test pitfalls:
- Building (or a coverage build) re-links into `bin/` and can confuse ninja timestamps — if a test run executes stale code, delete the stale exe in `bin/` first.
- A test may segfault once immediately after a build finishes; rerun before investigating.
- Remote tests (mysql/pg/dm) require the docker test servers; plain-name runs, `local`/`utils`/`pool`/`json` work offline.

## Layout (include/ is the public surface — keep it minimal)

```
src/include/        # 对外 public API, 4 headers only:
                    #   Idb.h (frozen interface), DbBase.h (factory decl),
                    #   GlobalConstants.h (status codes), dll_global.h (ZORM_API)
src/                # DbBase.cpp (the ONLY TU that sees backend headers),
                    #   GlobalConstants.cpp, main.cpp (demo)
src/base/           # private shared algorithm layer:
                    #   SqlBackendBase.h/.cpp, DbConnection.h, DbUtils.h/.cpp,
                    #   DbPool.h (template → header-only by necessity)
src/backends/       # private per-backend .h/.cpp pairs + FileLock.h/.cpp,
                    #   JsonFileDb.h/.cpp, pg_type_d.h (libpq oid table)
```

## Architecture

- **`SqlBackendBase`** (`src/base/SqlBackendBase.h/.cpp`) is an abstract base (no CRTP anymore) holding the 8 `Idb` method skeletons, SQL builders, `genSql` smart-query assembly (reserved words: page/size/sort/fuzzy/lks/ins/ors/count/sum/group), pagination counters, and the transaction loop — compiled once in `SqlBackendBase.cpp`. Backends drive the driver through the **`IDbConnection`** interface (`src/base/DbConnection.h`): each backend defines a small `Connection` class wrapping its native handle, pooled via `DbPool::HandlePool<IDbConnection*>` with RAII `Lease` (blocking wait, `invalidate` self-heal).
- **Dialect = virtual hook overrides.** Base defaults cover the plain-SQL majority (`?` placeholders, identity quoting, `limit o,n`, comma-join projections, `ON CONFLICT ... excluded` upsert, quote-doubling escaping). Similar backends share defaults naturally — sqlite needs ZERO overrides; postgres overrides 5 (placeholders `$n`, `numberedPlaceholders`, LIKE CAST, limit syntax, `$` sniffing); mysql overrides upsert + escaping; dm8 carries the quoted-lowercase identifier group plus read-then-write upsert overrides of `create`/`insertBatch`. Adding a backend = writing a `Connection` + overriding only the hooks that differ.
- **jsonfile backend** (`JsonFileDb.h/.cpp` + `FileLock.h/.cpp`, ~2.5k lines) is independent of the SQL stack: single JSON-file storage with cross-process file lock, atomic writes, corrupt-file backup, O(1) per-table id index. The `JsonFile::detail` helpers stay header-only (inline) because `tests/test_jsonfile.cpp` uses them directly.
- **Compiler flags matter**: `-fsigned-char` is set project-wide for GCC/Clang (ARM `char` is unsigned by default) — keep it.
- `refer/` (gels, orm) are read-only reference projects used as design precedents, not part of the build.

## zjson JSON-sniffing convention (important)

`Json(text)` / `add(key, text)` **parse** text beginning with `{` or `[` into objects/arrays. When a *value* is text that merely looks like JSON (`"[1,2]"`, `"{\"a\":1}"`), you must build it with `Json::str(text)` — otherwise type corruption. Bare-string keys/options keep the sniffing semantics by design. History: violations of this in the SQL decode paths and zjson's `extendItem` caused a real bug class (fixed in commit `8ade270`). Note also: zjson defines `ZJSON::string`/`ZJSON::vector` — in internal headers keep `std::` explicit.

## Testing conventions (contract suite)

- `tests/ContractSuite.h` defines one suite (Contract.Read/Write/Query/Dao, EdgeCases, MetadataCatalog, PlaceholderSql, TypeFidelity, EscapingFidelity) instantiated per backend via `ZORM_CONTRACT_TESTS()`; each backend test provides an `Env` subclass (connection options + DDL schema + dialect overrides like `rawSelectAll`). Assertions cover the **intersection** of the five backends' behavior; dialect-specific behavior goes in per-backend tests.
- `parameterized=false` (plain) is a separate code path (literal SQL generation + escaping + non-parameterized decode) — every SQL dialect is registered twice. New dialect → also register its `--no-param` variant.
- Backend-independent units get their own offline tests: `tests/test_dbutils.cpp` (DbUtils + DbBase facade validation, incl. the unknown-dbType throw) and `tests/test_pool.cpp` (HandlePool lease exclusivity, blocking checkout, invalidate self-heal, move semantics).
- Adding a column to the contract table → update `tests/dbconfig.json` (all dialect blocks), the embedded DDL in `tests/test_jsonfile.cpp`, and the TypeFidelity assertions, together.
- Regression gates: backend change → `./run-test all`; change to decode/SQL construction → verify with a **mutation** (revert the fix, confirm tests fail, then re-apply — a mutation that doesn't fail means the mutation was ineffective, not that coverage is absent).
- DM8 test server runs CASE_SENSITIVE=Y: unquoted identifiers in raw SQL fold to uppercase; Dm8Db-generated SQL is always quoted-lowercase — keep raw-SQL test hooks quoted.
