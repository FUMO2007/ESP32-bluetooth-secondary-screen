@echo off
rem ============================================================
rem  watch_net launcher  (ASCII only on purpose: cmd reads .bat
rem  with the OEM code page, so any non-ASCII text here breaks)
rem  Double-click to run. Ctrl+C to stop. No admin needed.
rem ============================================================
setlocal
set "PY="
for /d %%D in ("D:\PC_Monitor\*") do (
  if exist "%%~fD\venv\Scripts\python.exe" set "PY=%%~fD\venv\Scripts\python.exe"
)
if not defined PY (
  echo [ERROR] python venv not found under D:\PC_Monitor\*\venv\Scripts\
  pause
  exit /b 1
)
set PCMON_NO_HIDE=1
"%PY%" -B "%~dp0watch_net.py"
echo.
echo [exited] press any key to close...
pause >nul
