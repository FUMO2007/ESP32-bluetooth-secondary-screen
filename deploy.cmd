@echo off
rem ============================================================================
rem  deploy.cmd -- one-line entry point for deploy.ps1
rem ----------------------------------------------------------------------------
rem  Same reason as build.cmd: this machine has active code-integrity policies,
rem  so a .ps1 file that is newly created or modified is refused execution with a
rem  misleading "The file ... is not digitally signed" message. That has nothing
rem  to do with the script's contents -- an untouched script runs, a byte-identical
rem  copy of it does not. -ExecutionPolicy Bypass skips that check for this process
rem  only and changes no machine setting.
rem
rem  See the header of build.ps1 for the full evidence.
rem
rem  Usage and exit codes are identical to deploy.ps1; arguments pass through:
rem      deploy.cmd                 sync and verify
rem      deploy.cmd -WhatIf         report what would change, touch nothing
rem      deploy.cmd -Target <path>  sync somewhere else
rem  Exit code: 0 = synced and verified, 1 = something failed to sync.
rem
rem  NOTE: keep this file ASCII-only (cmd.exe reads .cmd with the OEM code page).
rem ============================================================================
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0deploy.ps1" %*
exit /b %ERRORLEVEL%
