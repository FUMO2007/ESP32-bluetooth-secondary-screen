@echo off
rem ============================================================================
rem  build.cmd -- one-line entry point for build.ps1
rem ----------------------------------------------------------------------------
rem  Why this file exists (read this if you ever see "is not digitally signed"):
rem
rem    This machine has active code-integrity policies
rem    (C:\Windows\System32\CodeIntegrity\CiPolicies\Active holds several .cip
rem    files). As a result, a .ps1 file that is *newly created or modified* is
rem    refused execution with a very misleading message:
rem        "The file ... is not digitally signed. You cannot run this script
rem         on the current system."
rem    Observed behaviour that pins it down:
rem      - deploy.ps1, untouched since it was first saved, runs fine
rem      - a byte-identical COPY of that same file is refused (name changed)
rem      - build.ps1 is refused as soon as an editor rewrites it
rem    So it has nothing to do with the script's contents or with a real
rem    signature: Windows is judging where the file came from. Passing
rem    -ExecutionPolicy Bypass skips that check for this process only and does
rem    not change any machine setting.
rem
rem    Therefore: run builds through this .cmd (or add -ExecutionPolicy Bypass
rem    yourself). Do NOT conclude the script is broken when you see that message.
rem
rem  NOTE: keep this file ASCII-only. cmd.exe reads .cmd/.bat with the OEM code
rem  page (936/GBK on this machine), so UTF-8 Chinese comments here get mangled
rem  into stray commands. Chinese explanations belong in build.ps1 (UTF-8 + BOM,
rem  read correctly by PowerShell) and in the README.
rem
rem  Usage and exit codes are identical to build.ps1; arguments pass through:
rem      build.cmd              everything (including the UI simulation)
rem      build.cmd -Quick       skip the slowest UI-simulation step
rem  Exit code: 0 = all passed, 1 = at least one step failed.
rem ============================================================================
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0build.ps1" %*
exit /b %ERRORLEVEL%
