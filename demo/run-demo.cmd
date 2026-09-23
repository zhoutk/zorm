@echo off
rem run-demo.cmd - one-command launcher for the Zorm Analytics MCP demo.
rem Sets up the zorm runtime DLL path, then runs the self-driven demo.
rem Usage:
rem   run-demo.cmd              rem 自动模式 (默认)
rem   run-demo.cmd --auto false rem 分步模式: 每步暂停等按键 + 验证提示
rem 其余参数原样透传给 zorm_analytics_server --demo。
setlocal
set SCRIPT_DIR=%~dp0
set ROOT=%SCRIPT_DIR%..
set PATH=%ROOT%\thirds\bin\win10;%PATH%

if not exist "%SCRIPT_DIR%..\build\demo\zorm_analytics_server.exe" (
  echo [demo] zorm_analytics_server.exe not found - build first:
  echo   cmake --preset clang-dbg ^&^& cmake --build build --target zorm_analytics_server
  exit /b 1
)

"%SCRIPT_DIR%..\build\demo\zorm_analytics_server.exe" --setup
if errorlevel 1 exit /b 1
"%SCRIPT_DIR%..\build\demo\zorm_analytics_server.exe" --demo %*
endlocal
