@echo off
cd /d "%~dp0"
if not exist "build\bin\Release\Whimsical.exe" (
  echo Run node tools/bootstrap.mjs then powershell -File tools/build.ps1 -Test
  pause
  exit /b 1
)
"build\bin\Release\Whimsical.exe" %*
