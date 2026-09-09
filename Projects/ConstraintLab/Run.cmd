@echo off
cd /d "%~dp0..\.."
build\bin\Release\Whimsical.exe --project Projects/ConstraintLab --width 1440 --height 900 %*
