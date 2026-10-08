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

Tests are config-driven (gels-style): one contract suite (`tests/ContractSuite.h`), backend selected by `--dialect` / `tests/dbconfig.json`. Run through `./run-test`, which sets up DLL paths (`thirds/bin/win10`) and maps short names to CTest registrations (13 total). The MCP layer has its own e2e tests (`mcp_test_zorm_analytics`, `mcp_test_zorm_mcp`) that spawn real server children — they run with plain `ctest -R mcp` (DLLs are copied next to the exes at build time; `run-test` is not involved).

The three MCP server exes (`zorm_mcp_server`, `zorm_analytics_server`, `zorm_server`) get their **full runtime DLL closure** copied next to them by a POST_BUILD step (`cmake/zorm_bundle_runtime_deps.cmake`, driven by `zorm_bundle_runtime_deps()` in the root CMakeLists). An AI client spawns the server with the client's own environment — no MSYS2 PATH — so without this the child dies at load with `Process exited with code 3221225781` (0xC0000135 `STATUS_DLL_NOT_FOUND`): the backend DLLs alone are not enough, `libc++.dll` / `libmariadb.dll` and their transitive deps come from `D:/msys64/clang64/bin`. Never "fix" a start failure by adding the toolchain bin to a client config — fix the bundling.

The two Python MCP servers in the VS Code `.vscode/mcp.json` (`serena`, `code-review-graph` — tools living outside this repo) run from uv-created venvs that deliberately keep **no state on `C:`** (only `D:` survives an OS reinstall): `uv.exe` is `D:\_sys_\uv\uv.exe`, the managed CPython is `D:\_sys_\uv\python\cpython-3.13.16-windows-x86_64-none` (no minor-version alias junction), and the venvs are `D:\_sys_\serena` / `D:\_sys_\code-review-graph`. Persisted user env: `UV_PYTHON_INSTALL_DIR=D:\_sys_\uv\python`, `UV_CACHE_DIR=D:\_sys_\uv\cache`. The venv launchers resolve the interpreter through `pyvenv.cfg`, so re-pointing a venv after a Python change is `uv venv --allow-existing --python 3.13.16 <venv>` (console scripts embed `<venv>\Scripts\python.exe`, so they survive as-is).

The VS Code **extension** `tirth8205.code-review-graph` (installed at `D:\_sys_\.vscode\extensions\tirth8205.code-review-graph-0.1.1`; `C:\Users\zhoutk\.vscode` is a symlink into `D:\_sys_\.vscode`) loads a native `better-sqlite3` addon, so it is pinned to the extension host ABI: VS Code 1.141 = Electron 43.7.7 = `NODE_MODULE_VERSION` **148**, while the marketplace build (0.1.1, 2026-03) ships an **ABI 146** binary → `activate()` throws at `new Database(...)` and every `codeReviewGraph.*` command answers "command not found". Repair (documented for the next VS Code upgrade): read the required ABI with `ELECTRON_RUN_AS_NODE=1 "<VS Code>/Code.exe" -e "console.log(process.versions.modules)"`, then install the extension's `node_modules/better-sqlite3` for that ABI — GitHub tag sources (`archive/refs/tags/v12.12.0.tar.gz`; note **12.12.0 was never published to npm**) plus the prebuilt `build/Release/better_sqlite3.node` from `releases/download/v12.12.0/better-sqlite3-v12.12.0-electron-v148-win32-x64.tar.gz`; verify by requiring that module with the same `ELECTRON_RUN_AS_NODE` trick against `D:\codes\zorm\.code-review-graph\graph.db`.

That extension **shells out to the Python CLI** for every build/update (`execFile(cliPath, ["build"|"update"], {cwd: <workspace>, timeout: 60000})`), and `CliWrapper.getCliPath()` is only `codeReviewGraph.cliPath` (user settings) or the bare name `code-review-graph` resolved from PATH — there is **no bundled CLI**. So the CLI must be reachable: user settings carry `"codeReviewGraph.cliPath": "D:\\_sys_\\code-review-graph\\Scripts\\code-review-graph.exe"` and the user PATH also has `D:\_sys_\code-review-graph\Scripts`. A full `build` of this repo takes ~25-50 s, inside the 60 s timeout; the CLI dir on PATH also makes `code-review-graph build|update|status` usable from any terminal. The extension's `dist/extension.js` is locally patched (registers the missing `codeReviewGraph.codeGraph.refresh` command, backup in `dist/extension.js.bak`) — re-apply after a marketplace update.

```bash
./run-test                # default: sqlitemem + jsonfile hardening + dbutils + pool
./run-test local          # sqlitemem + sqlite3(file) + json(file) + dbutils + pool
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
cmake/              # build helpers (private): zorm_bundle_runtime_deps.cmake
                    #   copies each MCP server's runtime DLL closure next to it
demo/               # validation/demo: zorm_analytics_server (multi-backend MCP
                    #   demo), demo_data seeding, run-demo scripts, publish pack
zorm-mcp/           # ★ the production MCP server (zorm_mcp_server): all Idb
                    #   methods + schema/SQL/transaction tools, MRTR confirm,
                    #   resources/prompts/completion; see zorm-mcp/README.md
                    #   and zorm-mcp/INTEGRATION.md (per-client setup)
```

## Architecture

- **`SqlBackendBase`** (`src/base/SqlBackendBase.h/.cpp`) is an abstract base (no CRTP anymore) holding the 8 `Idb` method skeletons, SQL builders, `genSql` smart-query assembly (reserved words: page/size/sort/fuzzy/lks/ins/ors/count/sum/group), pagination counters, and the transaction loop — compiled once in `SqlBackendBase.cpp`. Backends drive the driver through the **`IDbConnection`** interface (`src/base/DbConnection.h`): each backend defines a small `Connection` class wrapping its native handle, pooled via `DbPool::HandlePool<IDbConnection*>` with RAII `Lease` (blocking wait, `invalidate` self-heal). On a driver-level **write failure** the base calls the `selfHealConnections()` hook (virtual, default no-op; `Sqlit3Db` overrides it to `pool.invalidate()`) so a wedged handle (e.g. a database file that was transiently read-only) is dropped and rebuilt on the next acquire instead of failing forever.
- **Dialect = virtual hook overrides.** Base defaults cover the plain-SQL majority (`?` placeholders, identity quoting, `limit o,n`, comma-join projections, `ON CONFLICT ... excluded` upsert, quote-doubling escaping). Similar backends share defaults naturally — sqlite needs ZERO overrides; postgres overrides 5 (placeholders `$n`, `numberedPlaceholders`, LIKE CAST, limit syntax, `$` sniffing); mysql overrides upsert + escaping; dm8 carries the quoted-lowercase identifier group plus read-then-write upsert overrides of `create`/`insertBatch`. Adding a backend = writing a `Connection` + overriding only the hooks that differ.
- **jsonfile backend** (`JsonFileDb.h/.cpp` + `FileLock.h/.cpp`, ~2.5k lines) is independent of the SQL stack: single JSON-file storage with cross-process file lock, atomic writes, corrupt-file backup, O(1) per-table id index. The `JsonFile::detail` helpers stay header-only (inline) because `tests/test_jsonfile.cpp` uses them directly.
- **MCP servers** live on top of the frozen `Idb` surface: `zorm-mcp/` (production `zorm_mcp_server`: 13 tools covering all 8 Idb methods + catalog-backed schema metadata + read-only SQL gate + transactions, every write behind an MRTR `input_required`/`requestState` confirmation) and `demo/` (validation showcase). Both link `thirds/mcp-cpp`, which is protocol-faithful — do NOT modify it for app needs (protocol text: `docs/mcp/`). Schema metadata comes from per-dialect catalogs (sqlite pragma TVFs — bare `pragma` breaks the backend's ` limit 1` column probe — / information_schema / user_tab_columns / the jsonfile columns array), never by querying rows. Multi-backend routing: every tool takes a `db` param naming a `DbBase` instance from `--config` (built-in: sqlite3 + jsonfile).
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
