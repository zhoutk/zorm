#!/bin/sh
# =============================================================================
# publish-mcp.sh - 发布 zorm_analytics_server MCP 服务（免 MSYS2 自包含包）
#
# 把 zorm_analytics_server.exe 连同全部运行时依赖打包成一个自包含目录，
# 在没有安装 MSYS2 / 没有任何第三方依赖的机器上也能直接注册为 MCP 服务。
#
# 依赖收集策略：
#   - 以 MSYS2 的 ldd 为依赖收集器，对 exe 做完整递归闭包解析
#     （libc++.dll / libmariadb -> libcurl-4 -> libssl-3 -> libnghttp2/...
#      libpq -> libssl-1_1/libintl-9/...  dmdpi -> dm*.dll 全家 全部自动收齐）；
#   - 系统 DLL（KERNEL32 / ntdll / api-ms-win-crt-* / VCRUNTIME140 / bcrypt /
#     WS2_32 ...）每台 Windows 都有，跳过不打包；
#   - 收集到的每个 DLL 再递归扫描其自身依赖，直到闭包稳定
#     （ldd 一次输出已含完整闭包，这里对每条依赖再做一次校验式深扫，
#      防 ldd 解析顺序导致的个别遗漏）。
#
# 产出目录（默认 <脚本所在目录>/zorm-mcp，--dir 可指定）：
#   zorm-mcp/
#     start.cmd / start.sh       启动脚本（自动 cd 到发布目录再跑 bin/ 下 exe，
#                                 MCP 客户端注册它即可，无需配 cwd）
#     zorm_analytics.json         MCP 服务配置（默认 sqlite+jsonfile，可加
#                                 mysql/postgres/dm8 接入点）
#     README.md                   在目标机器上的注册/使用说明
#     bin/
#       zorm_analytics_server.exe   MCP 服务本体（stdio）
#       *.dll                       全部运行时依赖
#
# 发布后使用（目标机器无 MSYS2）：
#   1) 拷贝 zorm-mcp 目录到目标机器（任意位置）；
#   2) 首次运行：start.cmd --setup   （建库+样例数据，幂等）
#   3) MCP 客户端注册（Claude Code / VS Code / DSH ...）：
#        command = <zorm-mcp 目录>/start.cmd   （自动 cd 到发布目录，
#                  demo.db / demo.json 落在发布目录，无需配 cwd）
#
# 用法：
#   ./demo/publish-mcp.sh                 # 发布到 <脚本目录>/zorm-mcp
#   ./demo/publish-mcp.sh --dir D:/x/zorm-mcp
#   ./demo/publish-mcp.sh --exe build/demo/zorm_analytics_server.exe  # 自定义 exe 路径
#   ./demo/publish-mcp.sh --smoke false   # 跳过裸机冒烟验证
# =============================================================================
set -eu

# ---- 默认值 ---------------------------------------------------------------
SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
ROOT="$SCRIPT_DIR/.."
EXE="$ROOT/build/demo/zorm_analytics_server.exe"
OUT_DIR="$SCRIPT_DIR/zorm-mcp"
SMOKE=1

# ---- 参数解析 -------------------------------------------------------------
while [ $# -gt 0 ]; do
  case "$1" in
    --dir)   OUT_DIR="$2"; shift 2 ;;
    --exe)   EXE="$2"; shift 2 ;;
    --smoke) SMOKE="$2"; shift 2 ;;
    -h|--help)
      sed -n '2,20p' "$0" | sed 's/^# \{0,1\}//'
      echo "用法: $0 [--dir <发布目录>] [--exe <server路径>] [--smoke true|false]"
      exit 0 ;;
    *) echo "[publish] 未知参数: $1" >&2; exit 2 ;;
  esac
done

EXE=$(cd "$(dirname "$EXE")" && pwd)/$(basename "$EXE")
OUT_DIR=$(cygpath -u "$OUT_DIR" 2>/dev/null || echo "$OUT_DIR")
[ "$SMOKE" = "true" ] || [ "$SMOKE" = "1" ] || SMOKE=0

echo "==  zorm MCP 发布  =="
echo "   server : $EXE"
echo "   发布到 : $OUT_DIR"

# ---- 前置检查 -------------------------------------------------------------
[ -f "$EXE" ] || { echo "[publish] 找不到 server: $EXE （先构建: cmake --preset clang-dbg && cmake --build build --target zorm_analytics_server）" >&2; exit 1; }
command -v ldd >/dev/null 2>&1 || { echo "[publish] 需要 MSYS2 的 ldd 做依赖收集（本机 MSYS2 自带）" >&2; exit 1; }
file "$EXE" | grep -qi "PE32+.*x86-64" || { echo "[publish] 只支持 64 位 Windows 发布（PE32+ x86-64）" >&2; exit 1; }

# ---- 依赖收集：递归闭包 -----------------------------------------------------
# ldd 输出形如 "  foo.dll => /path/foo.dll (0x...)  或  /path/foo.dll (0x...)"
# 系统 DLL（/c/Windows 与 api-ms-win-*）不打包。
# 注意：ldd 在 MSYS2 下对部分 PE 的 /c/Windows 前缀可能输出为 C:\Windows 或
# /cygdrive/...，统一转成 MSYS 路径再判断。

collect() {  # collect <file>  -> 追加其全部非系统依赖到 DEP_FILES（全局集合）
  local f="$1"
  [ -f "$f" ] || return 0
  local line target dll
  ldd "$f" 2>/dev/null | while IFS= read -r line; do
    target=$(echo "$line" | sed -n 's/.*=> \(.*\) (0x.*)/\1/p')
    [ -z "$target" ] && target=$(echo "$line" | sed -n 's/^[[:space:]]*\([^ (]*\.dll\)[[:space:]]*(0x.*/\1/p')
    [ -z "$target" ] && continue
    # MSYS 路径归一化：cygpath 失败（普通 cmd 调用）时保留原样
    dll=$(cygpath -u "$target" 2>/dev/null || echo "$target")
    case "$dll" in
      /c/Windows/*|C:/Windows/*|C:\\Windows\\*) continue ;;  # 系统 DLL
      /clang64/*|/mingw64/*|/usr/*) : ;;                     # 允许的第三方前缀
    esac
    [ -f "$dll" ] || continue
    echo "$dll"
  done
}

DEP_FILES=""
seen=""
new=""
queue="$EXE"
echo "==  递归解析依赖（ldd）=="
while [ -n "$queue" ]; do
  for f in $queue; do
    case " $seen " in *" $f "*) continue ;; esac
    seen="$seen $f"
    echo "  [scan] $(basename "$f")"
    out=$(collect "$f")
    for d in $out; do
      case " $DEP_FILES " in *" $d "*) continue ;; esac
      DEP_FILES="$DEP_FILES $d"
      new="$new $d"
      echo "      -> $(basename "$d")"
    done
  done
  queue="$new"; new=""
done

[ -n "$DEP_FILES" ] || { echo "[publish] 未收集到任何依赖（异常）" >&2; exit 1; }

echo "==  依赖清单（$(echo $DEP_FILES | tr ' ' '\n' | sort -u | wc -l) 个非系统 DLL）=="
echo "$DEP_FILES" | tr ' ' '\n' | sort -u | sed 's/^/    /'

# ---- 组装发布目录 -----------------------------------------------------------
echo "==  组装发布目录 $OUT_DIR =="
rm -rf "$OUT_DIR"
mkdir -p "$OUT_DIR/bin"

echo "  [copy] $(basename "$EXE")"
cp "$EXE" "$OUT_DIR/bin/"
for d in $(echo "$DEP_FILES" | tr ' ' '\n' | sort -u); do
  echo "  [copy] $(basename "$d")"
  cp -f "$d" "$OUT_DIR/bin/"
done

# 生成启动脚本：cd 到发布目录再启动 bin/ 下的 server。
# MCP 客户端注册 command 指向本脚本即可，无需手工配 cwd——
# demo.db / demo.json 永远落在发布目录。
cat > "$OUT_DIR/start.cmd" <<'CMD'
@echo off
rem start.cmd - launch zorm_analytics_server MCP server.
rem cd to this script's dir (the release dir), then run bin\zorm_analytics_server.exe,
rem so demo.db / demo.json always land in the release dir; MCP clients need no cwd.
rem usage: start.cmd [--setup] [--config zorm_analytics.json] [...]
setlocal
cd /d "%~dp0"
"%~dp0bin\zorm_analytics_server.exe" %*
endlocal
CMD

cat > "$OUT_DIR/start.sh" <<'SH'
#!/bin/sh
# start.sh - 启动 zorm_analytics_server MCP 服务（MSYS2/Git Bash 版）。
# 自动 cd 到本脚本所在目录，再执行 bin/zorm_analytics_server.exe。
# 用法: ./start.sh [--setup] [--config zorm_analytics.json] [...]
cd "$(dirname "$0")" || exit 1
exec bin/zorm_analytics_server.exe "$@"
SH
chmod +x "$OUT_DIR/start.sh"

# 生成 MCP 服务配置文件（默认 sqlite+jsonfile，注释展示 mysql/pg/dm8 接入）
cat > "$OUT_DIR/zorm_analytics.json" <<'JSON'
{
  "databases": {
    "sqlite3":  { "type": "sqlite3",  "options": { "connString": "demo.db" } },
    "jsonfile": { "type": "jsonfile", "options": { "connString": "demo.json" } }
  }
}
JSON

cat > "$OUT_DIR/README.md" <<'MD'
# Zorm Analytics MCP — 自包含发布包

无需安装 MSYS2 / 任何第三方依赖，拷贝即用。

```
zorm-mcp/
  start.cmd / start.sh      启动脚本（MCP 客户端注册它即可，自动 cd 到本目录）
  zorm_analytics.json        MCP 服务配置（默认 sqlite+jsonfile，可加 mysql/pg/dm8）
  bin/
    zorm_analytics_server.exe   MCP 服务本体（stdio）
    *.dll                       全部运行时依赖
```

## 快速开始

```cmd
cd <发布目录>
start.cmd --setup     :: 建库+样例数据（幂等，仅首次需要）
```

启动脚本会 `cd` 到发布目录再运行 `bin\zorm_analytics_server.exe`，
因此 `demo.db` / `demo.json` 始终落在**发布目录**（不是 bin/ 下）。

## 注册为 MCP 服务

**command** 统一指向 `start.cmd`（Windows）或 `start.sh`（MSYS2/Git Bash），**无需配置 cwd**——脚本会自动切到发布目录。

### DeepSeek Harness（dsh）

DSH 使用 MCP SDK 2.0（协议 2026-07-28），是**严格客户端**：`tools/list` 等结果缺 `ttlMs`/`cacheScope` 会直接拒绝（表现为 `Invalid result for tools/list: ttlMs expected number` → `server is disconnected`）。本包为最新构建，mcp-cpp 已默认注入这两个字段。

编辑 `C:\Users\<user>\.dsh\profiles\web\cordis.patch.yml`，在数组末尾追加：

```yaml
- insert:
    - id: mcp-zorm-analytics
      name: '@deepseek-ai/dsh-mcp-client'
      config:
        serverName: zorm-analytics
        transport: stdio
        command: <发布目录>/start.cmd
        cwd: <发布目录>
```

保存即生效（profile 默认 `patchReload: "live"` 热加载）。日志出现 `mcp-client(zorm-analytics)` 即注册成功；若见 `server is disconnected`，先确认用的是本发布包的最新 exe。

会话里直接问：

> 根目录下的 demo.db 中有哪些表？

> 把 sqlite 里订单表的 2026 年订单按地区分组汇总金额，结果存到 jsonfile 的 analytics 表。

验证：`tools/list` 应列出 6 个工具（`mcp__zorm-analytics__db_list_tables` 等）；写入工具触发 MRTR `input_required` 确认。

### Claude Code
```bash
claude mcp add zorm-analytics -- <发布目录>/start.cmd
```

### VS Code（.vscode/mcp.json 或用户 settings.json）
```json
{ "mcp": { "servers": { "zorm-analytics": {
    "command": "<发布目录>/start.cmd"
} } } }
```

## 多后端接入（可选）

编辑 `zorm_analytics.json`，用 `start.cmd --config zorm_analytics.json` 启动：
```jsonc
{ "databases": {
    "sqlite3":  { "type": "sqlite3",  "options": { "connString": "demo.db" } },
    "jsonfile": { "type": "jsonfile", "options": { "connString": "demo.json" } },
    "mysql":    { "type": "mysql",    "options": { "db_host": "...", "db_user": "...", "db_pass": "...", "db_name": "...", "db_port": 3306 } },
    "postgres": { "type": "postgres", "options": { "db_host": "...", "db_user": "...", "db_pass": "...", "db_name": "...", "db_port": 5432 } },
    "dm8":      { "type": "dm8",      "options": { "db_host": "...", "db_user": "...", "db_pass": "..." } }
} }
```

## 常见问题

- **`--setup` 报缺 DLL**：发布目录 `bin/` 下应有 `libc++.dll`、`dmdpi.dll`、
  `sqlite3.dll`、`libmariadb.dll`、`LIBPQ.dll` 及它们的全部子依赖；若缺，
  在开发机上重新运行 `publish-mcp.sh`（它按 ldd 闭包自动收齐）。

- **`SQLITE_READONLY`，读正常但一写就报只读**：这是 Windows **完整性级别（Integrity
  Level）** 问题，不是文件只读、也不是 server bug。Claude Code 在 Windows 上以**低
  完整性（Low）**运行 MCP 服务器等原生子进程；低完整性进程**能读**普通（中完整性）
  文件、**不能写**。SQLite 写打开失败时静默降级为只读，直到真正写入才爆 `SQLITE_READONLY`。

  修复（PowerShell / cmd，**目录和文件都要**）：
  ```powershell
  icacls "<发布目录>" /setintegritylevel Low
  icacls "<发布目录>\demo.db" /setintegritylevel Low
  ```
  目录设 Low 是为了让 SQLite 能在同目录建 `demo.db-journal` 日志；已存在的文件不会
  自动继承目录新标签，必须单独设。验证：`icacls "<发布目录>\demo.db"` 应出现
  `Mandatory Label\Low Mandatory Level:(NW)` 一行，再重跑 `db_insert`。

  > MSYS2/Git Bash 里执行请加前缀防路径转换：
  > `MSYS2_ARG_CONV_EXCL='*' icacls 'D:\tmp' /setintegritylevel Low`
  > 可逆：把 `Low` 改回 `Medium` 即完全恢复，只改标签不动数据/权限。
MD

echo "==  发布完成 =="
du -sh "$OUT_DIR" | sed 's/^/    /'

# ---- 裸机冒烟验证 -----------------------------------------------------------
# 用「仅系统 PATH」模拟无 MSYS2 的目标机器：发布目录内的 exe 必须能独立跑通
# --setup（加载全部依赖 DLL、建库、seed 数据）。走 start.cmd 启动脚本，
# 与 README 注册方式完全一致。
smoke() {
  echo "==  冒烟验证（仅系统 PATH，模拟无 MSYS2 机器）=="
  local oldpath="$PATH"
  # Windows 系统 PATH（含 System32，保证 KERNEL32/api-ms-win-crt/VCRUNTIME140）
  PATH="$WINDIR/System32;$WINDIR;$WINDIR/System32/Wbem"
  export PATH
  if ( cd "$OUT_DIR" && ./start.cmd --setup ); then
    export PATH="$oldpath"
    echo "==  冒烟通过：--setup 成功（demo.db / demo.json 已生成）=="
    ls -la "$OUT_DIR"/demo.db "$OUT_DIR"/demo.json
  else
    export PATH="$oldpath"
    echo "[publish] 冒烟失败：请检查 bin/ 下依赖是否齐全（对比开发机 ldd 输出）" >&2
    exit 1
  fi
}
[ "$SMOKE" = "1" ] && smoke

echo "==  完成 =="
