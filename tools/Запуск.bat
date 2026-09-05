@echo off
cd /d "%~dp0"
if not exist Launcher.exe (
  echo Нет Launcher.exe
  pause
  exit /b 1
)
start "" "%~dp0Launcher.exe"
