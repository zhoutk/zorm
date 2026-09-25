@echo off
:: zorm-mcp launcher - MCP clients register THIS script as the command.
:: It pins the working directory to zorm-mcp/ so the default database files
:: (zorm-mcp.db / zorm-mcp.json) always land here, no matter where the client
:: spawns the server from. All arguments pass through to the server
:: (e.g. start.cmd --config zorm-mcp.json --allow-sql).
setlocal
cd /d "%~dp0"
set EXE=%~dp0..\build\zorm-mcp\zorm_mcp_server.exe
if not exist "%EXE%" (
  echo [zorm-mcp] server not found: %EXE% 1>&2
  echo [zorm-mcp] build it first: cmake --preset clang-rlz ^&^& cmake --build build --target zorm_mcp_server 1>&2
  exit /b 1
)
"%EXE%" %*
