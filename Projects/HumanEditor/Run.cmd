@echo off
cd /d "%~dp0..\.."
build\bin\Release\Afterlight.exe --project Projects/HumanEditor --width 1600 --height 1000 %*
