#!/usr/bin/env bash
# zorm-mcp launcher - MCP clients register THIS script as the command.
# It pins the working directory to zorm-mcp/ so the default database files
# (zorm-mcp.db / zorm-mcp.json) always land here, no matter where the client
# spawns the server from. All arguments pass through to the server.
set -euo pipefail
cd "$(dirname "$0")"
EXE="$(dirname "$0")/../build/zorm-mcp/zorm_mcp_server"
if [[ "$(uname -s)" == MINGW* || "$(uname -s)" == MSYS* || -f "${EXE}.exe" ]]; then
  EXE="${EXE}.exe"
fi
if [[ ! -x "$EXE" && ! -f "$EXE" ]]; then
  echo "[zorm-mcp] server not found: $EXE" >&2
  echo "[zorm-mcp] build it first: cmake --preset clang-rlz && cmake --build build --target zorm_mcp_server" >&2
  exit 1
fi
exec "$EXE" "$@"
