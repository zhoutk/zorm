#!/bin/sh
# run-demo.sh - one-command launcher for the Zorm Analytics MCP demo.
# Sets up the zorm runtime DLL path, then runs the self-driven demo.
# Usage:
#   ./run-demo.sh             # 自动模式 (默认)
#   ./run-demo.sh --auto false  # 分步模式: 每步暂停等按键 + 验证提示
# 其余参数原样透传给 zorm_analytics_server --demo。
SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
ROOT="$SCRIPT_DIR/.."

export PATH="$ROOT/thirds/bin/win10:$PATH"
EXE="$ROOT/build/demo/zorm_analytics_server.exe"

if [ ! -x "$EXE" ]; then
  echo "[demo] zorm_analytics_server not found - build first:"
  echo "  cmake --preset clang-dbg && cmake --build build --target zorm_analytics_server"
  exit 1
fi

"$EXE" --setup || exit 1
exec "$EXE" --demo "$@"
