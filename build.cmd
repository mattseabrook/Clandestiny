@echo off
setlocal

set "ROOT=%~dp0"
cd /d "%ROOT%"

set "ACTION=%~1"
if "%ACTION%"=="" set "ACTION=build"

if /I "%ACTION%"=="clean" goto clean
if /I "%ACTION%"=="rebuild" goto rebuild
if /I "%ACTION%"=="build" goto build
if /I "%ACTION%"=="release" goto build
if /I "%ACTION%"=="debug" goto debug
if /I "%ACTION%"=="help" goto usage
if /I "%ACTION%"=="-h" goto usage
if /I "%ACTION%"=="--help" goto usage

echo Unknown command: %ACTION%
goto usage_error

:build
set "OUT=groovie2.exe"
set "CLFLAGS=/nologo /std:c++20 /EHsc /MT /O2 /DNDEBUG /DWIN32_LEAN_AND_MEAN /DNOMINMAX /D_CRT_SECURE_NO_WARNINGS"
goto compile

:debug
set "OUT=groovie2-debug.exe"
set "CLFLAGS=/nologo /std:c++20 /EHsc /MT /Od /Zi /DWIN32_LEAN_AND_MEAN /DNOMINMAX /D_CRT_SECURE_NO_WARNINGS"
goto compile

:compile
where cl.exe >nul 2>nul
if errorlevel 1 (
    echo cl.exe was not found. Run this from a Developer PowerShell or Developer Command Prompt for Visual Studio.
    exit /b 1
)

echo Compiling %OUT%...
cl.exe %CLFLAGS% groovie2.cpp config.cpp /Fe:%OUT% /link /SUBSYSTEM:WINDOWS windowscodecs.lib ole32.lib uuid.lib user32.lib gdi32.lib shell32.lib comctl32.lib winmm.lib
if errorlevel 1 exit /b 1
echo Built %OUT%
exit /b 0

:clean
del /q groovie2.exe groovie2-debug.exe *.obj *.pdb *.ilk 2>nul
if exist build rmdir /s /q build
exit /b 0

:rebuild
call "%~f0" clean
if errorlevel 1 exit /b 1
call "%~f0" build
exit /b %ERRORLEVEL%

:usage
echo Usage: build.cmd [build^|debug^|clean^|rebuild]
echo.
echo Builds groovie2.exe as the Windows GUI executable. When command-line
echo arguments are supplied to groovie2.exe, it runs the CLI extractor instead.
exit /b 0

:usage_error
echo Usage: build.cmd [build^|debug^|clean^|rebuild]
exit /b 2
