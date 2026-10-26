@echo off
rem Build the Windows eJay 2 host: 32-bit, because the title's DLLs are, with
rem the C runtime linked in so it runs on a PC without the VC++ redistributable.
rem
rem   host\build.bat        -> build-windows\ejay2.exe
rem
rem Needs Visual Studio 2019 or newer (Community or Build Tools) with the C++
rem workload; vswhere finds it, wherever it is installed.
setlocal
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (echo Setup stopped: Visual Studio's C++ build tools are not installed. & exit /b 1)
set "VS="
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VS=%%i"
if not defined VS (echo Setup stopped: Visual Studio is installed without the C++ build tools. & exit /b 1)
set "PATH=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer;%PATH%"
call "%VS%\VC\Auxiliary\Build\vcvarsall.bat" x86 >nul || exit /b 1
cd /d "%~dp0.."
if not exist build-windows mkdir build-windows
cl /nologo /W3 /O2 /MT /D_CRT_SECURE_NO_WARNINGS /Fo"build-windows\\" host\ejay2.c host\win32.c /Fe"build-windows\ejay2.exe" ^
   user32.lib gdi32.lib winmm.lib ole32.lib /link /MAP:build-windows\ejay2.map || exit /b 1
rem The map turns the "ejay2.exe + offset" a crash report prints into a function.
