@echo off
REM Double-clickable wrapper for GenerateVS2026.ps1 (bypasses PowerShell's
REM default execution policy just for this one script invocation).
REM Any arguments passed to this .bat are forwarded as-is, e.g.:
REM   GenerateVS2026.bat -Clean
REM   GenerateVS2026.bat -SkipSubmodules
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0GenerateVS2026.ps1" %*
exit /b %ERRORLEVEL%
