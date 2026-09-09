@echo off
cd /d "%~dp0..\.."
build\bin\Release\Whimsical.exe --project Projects/HumanEditor --width 1600 --height 1000 %*
