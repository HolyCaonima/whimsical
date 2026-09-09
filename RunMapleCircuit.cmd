@echo off
cd /d "%~dp0"
"build\bin\Release\Whimsical.exe" --project "Projects\MapleCircuit" --width 1280 --height 800 %*
