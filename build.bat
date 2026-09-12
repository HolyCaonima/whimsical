@echo off
setlocal
set "ROOT=%~dp0"
set "CONFIG=%~1"
if "%CONFIG%"=="" set "CONFIG=Release"
powershell -NoProfile -ExecutionPolicy Bypass -File "%ROOT%tools\build.ps1" -Configuration %CONFIG% %~2
set "CODE=%ERRORLEVEL%"
echo.
echo Build exited with code %CODE%
pause
exit /b %CODE%
