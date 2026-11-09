@echo off
setlocal
cd /d "%~dp0"
where cl >nul 2>nul
if errorlevel 1 (
    echo Run this from an x86 or x64 Native Tools Command Prompt for Visual Studio.
    exit /b 1
)
cl /nologo /O2 /W4 /std:c++17 /EHsc /utf-8 /DUNICODE /D_UNICODE FM_Win32.cpp vendor\opl3.c /Fe:FM_Tracker.exe /link /SUBSYSTEM:WINDOWS /STACK:8388608
if errorlevel 1 exit /b 1
