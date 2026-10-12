@echo off
rem ============================================================
rem  watch_net launcher  (ASCII only on purpose: cmd reads .bat
rem  with the OEM code page, so any non-ASCII text here breaks)
rem  Double-click to run. Ctrl+C to stop. No admin needed.
rem
rem  Python is located relative to THIS file's repository, not by
rem  hardcoding a machine-specific path -- the old version only
rem  looked under D:\PC_Monitor\*\venv, so a clone elsewhere
rem  could not run it at all.
rem ============================================================
setlocal
set "PY="
set "ROOT=%~dp0.."

rem 1) a venv inside the repository (either layout)
for %%V in ("%ROOT%\.venv" "%ROOT%\venv" "%ROOT%\..\PC端\venv") do (
  if not defined PY if exist "%%~fV\Scripts\python.exe" set "PY=%%~fV\Scripts\python.exe"
)

rem 2) fall back to whatever "python" resolves to on PATH
if not defined PY for /f "delims=" %%P in ('where python 2^>nul') do (
  if not defined PY set "PY=%%P"
)

if not defined PY (
  echo [ERROR] no Python found. Looked for a venv in the repo and for
  echo         "python" on PATH. Create one with:
  echo             python -m venv .venv
  echo             .venv\Scripts\pip install -r requirements.txt
  pause
  exit /b 1
)
set PCMON_NO_HIDE=1
"%PY%" -B "%~dp0watch_net.py"
echo.
echo [exited] press any key to close...
pause >nul
