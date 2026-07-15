@echo off
setlocal
cd /d "%~dp0"
rem Keep default code page (GBK on Chinese Windows). Do NOT use chcp 65001.
powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%~dp0copybin.ps1"
set ERR=%ERRORLEVEL%
echo.
pause
exit /b %ERR%
