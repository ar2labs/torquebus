@echo off
rem SPDX-License-Identifier: GPL-3.0-or-later
rem
rem A build prompt with BOTH halves of the environment TorqueBus needs.
rem
rem This exists because qtenv2.bat alone is not enough, and the way it fails is
rem misleading. qtenv2.bat sets QTDIR and puts Qt on PATH - nothing else. It
rem says so itself on startup ("Remember to call vcvarsall.bat to complete
rem environment setup!"), and that line is easy to scroll past.
rem
rem Without vcvars there is no INCLUDE, no LIB, and no MSVC linker ahead of
rem whatever else is on PATH. CMake then finds cl.exe (often still on PATH from
rem a system-wide entry), compiles its test file happily, and links it with the
rem first ld.exe it can find - a GNU linker, from an embedded toolchain such as
rem STM32CubeCLT. The result is a wall of "cannot find /nologo" and "cannot
rem find kernel32.lib", which reads like a broken Windows SDK and is not.
rem
rem Order matters only in one direction: vcvars first, so that Qt's bin ends up
rem ahead of it on PATH and windeployqt resolves to the Qt we are building for.
rem
rem Which Qt: QTDIR when it names a Qt 6 installation, the installer default
rem otherwise. That is the order cmake/TorqueBusDependencies.cmake looks in, so
rem the prompt and the configure cannot disagree about which Qt this is - and it
rem is how to use a Qt installed on another disk: set QTDIR, or edit TORQUEBUS_QT
rem below. A QTDIR that does not hold a Qt 6 is ignored and not trusted, because a
rem stale one - left by another project's prompt, say - should not beat a Qt that
rem is actually there.
rem
rem Usage:  tools\torquebus-prompt.bat
rem Then:   cmake --preset windows-msvc-debug

setlocal EnableDelayedExpansion

set "TORQUEBUS_QT=C:\Qt\6.11.2\msvc2022_64"
set "TORQUEBUS_QT_NOTE="
set "TORQUEBUS_VS="

rem One line of chained ifs, which is an AND, rather than a parenthesised block.
rem A path under "Program Files (x86)" has a ")" in it, and %VAR% inside a block
rem is expanded when the block is *parsed* - so an unquoted one ends the block
rem early even if that branch never runs, and the script dies with "was
rem unexpected at this time". Where a block has to print a path, it uses !VAR!,
rem which is expanded when the line runs. This is not hypothetical for the VS
rem path below: the Build Tools for Visual Studio 2022 install under
rem "Program Files (x86)".
if defined QTDIR if exist "%QTDIR%\bin\qtenv2.bat" if exist "%QTDIR%\lib\cmake\Qt6\Qt6Config.cmake" set "TORQUEBUS_QT=%QTDIR%"

if defined QTDIR if /i not "%TORQUEBUS_QT%"=="%QTDIR%" set "TORQUEBUS_QT_NOTE=QTDIR is set to %QTDIR%, but that has no bin\qtenv2.bat and lib\cmake\Qt6 - so it was ignored."

rem vswhere ships with every VS 2017+ installer, at a fixed path, and is the
rem supported way to locate an installation. Hard-coding "Community" would
rem break for anyone with Professional or a different drive.
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"

if exist "%VSWHERE%" (
    for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * ^
        -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 ^
        -property installationPath`) do set "TORQUEBUS_VS=%%i"
)

if not defined TORQUEBUS_VS (
    echo.
    echo   Visual Studio 2022 with the C++ toolset was not found.
    echo   Install "Desktop development with C++" from the Visual Studio Installer.
    echo.
    exit /b 1
)

if not exist "%TORQUEBUS_VS%\VC\Auxiliary\Build\vcvars64.bat" (
    echo.
    echo   Found Visual Studio at:
    echo     !TORQUEBUS_VS!
    echo   but not its vcvars64.bat, so the C++ toolset is not installed.
    echo.
    exit /b 1
)

if not exist "%TORQUEBUS_QT%\bin\qtenv2.bat" (
    echo.
    echo   Qt was not found at:
    echo     !TORQUEBUS_QT!
    if defined TORQUEBUS_QT_NOTE echo   !TORQUEBUS_QT_NOTE!
    echo   Set QTDIR to your Qt 6 installation - the folder that holds bin and lib,
    echo   for example C:\Qt\6.11.2\msvc2022_64 - or edit TORQUEBUS_QT at the top of
    echo   this file.
    echo.
    exit /b 1
)

endlocal & set "TORQUEBUS_VS=%TORQUEBUS_VS%" & set "TORQUEBUS_QT=%TORQUEBUS_QT%"

rem /K keeps the window open afterwards, which is the point of a prompt.
%SystemRoot%\System32\cmd.exe /A /Q /K ""%TORQUEBUS_VS%\VC\Auxiliary\Build\vcvars64.bat" ^&^& "%TORQUEBUS_QT%\bin\qtenv2.bat" ^&^& cd /d "%~dp0.." ^&^& echo. ^&^& echo   MSVC x64 + Qt 6.11.2 ready. Next: cmake --preset windows-msvc-debug ^&^& echo."
