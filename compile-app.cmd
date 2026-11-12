@echo off
REM The repository root is the directory holding this script, so a clone
REM anywhere builds without editing a path. %~dp0 ends in a backslash, which
REM the joins below would double.
setlocal
set "ROOT=%~dp0"
if "%ROOT:~-1%"=="\" set "ROOT=%ROOT:~0,-1%"
cd /d "%ROOT%\App"
C:\Tools\arduino-cli\arduino-cli.exe compile --fqbn esp32:esp32:esp32:PartitionScheme=min_spiffs --libraries "%ROOT%\libraries" --export-binaries . > "%ROOT%\app-compile.log" 2>&1
echo EXIT=%ERRORLEVEL% >> "%ROOT%\app-compile.log"
