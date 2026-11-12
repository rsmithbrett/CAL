@echo off
REM Detached so a 30-minute harness limit cannot kill a compile that now takes
REM longer than that under core contention. Writes its own sentinel at the end,
REM which is the only reliable signal that it finished rather than vanished.
REM
REM The repository root is the directory holding this script, so a clone
REM anywhere builds without editing a path. %~dp0 ends in a backslash, which
REM the joins below would double.
setlocal
set "ROOT=%~dp0"
if "%ROOT:~-1%"=="\" set "ROOT=%ROOT:~0,-1%"
del /q "%ROOT%\app-compile.log" 2>nul
del /q "%ROOT%\compile-done.flag" 2>nul
call "%ROOT%\compile-app.cmd"
echo FINISHED > "%ROOT%\compile-done.flag"
