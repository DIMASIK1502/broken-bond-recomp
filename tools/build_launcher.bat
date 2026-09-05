@echo off
setlocal
cd /d "%~dp0"
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VS=%%i"
if not defined VS (
  echo Visual Studio / MSVC not found
  exit /b 1
)
call "%VS%\VC\Auxiliary\Build\vcvars64.bat" >nul
cl /nologo /O2 /MT /EHsc /std:c++17 /utf-8 /W3 /Fe:Launcher.exe launcher_win.cpp /link /SUBSYSTEM:WINDOWS /INCREMENTAL:NO
if errorlevel 1 exit /b 1
del /q launcher_win.obj 2>nul
echo Built Launcher.exe
