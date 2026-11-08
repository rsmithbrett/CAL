@echo off
REM The repository root is the directory holding this script, so a clone
REM anywhere builds without editing a path. %~dp0 ends in a backslash, which
REM the joins below would double.
setlocal
set "ROOT=%~dp0"
if "%ROOT:~-1%"=="\" set "ROOT=%ROOT:~0,-1%"
del /q "%ROOT%\cal-compile.log" 2>nul
del /q "%ROOT%\cal-done.flag" 2>nul
cd /d "%ROOT%"
C:\Tools\arduino-cli\arduino-cli.exe compile --fqbn esp32:esp32:esp32:PartitionScheme=min_spiffs --libraries "%ROOT%\libraries" --export-binaries . > "%ROOT%\cal-compile.log" 2>&1
echo EXIT=%ERRORLEVEL% >> "%ROOT%\cal-compile.log"
echo FINISHED > "%ROOT%\cal-done.flag"
