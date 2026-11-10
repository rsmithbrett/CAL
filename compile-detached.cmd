@echo off
REM Detached so a 30-minute harness limit cannot kill a compile that now takes
REM longer than that under core contention. Writes its own sentinel at the end,
REM which is the only reliable signal that it finished rather than vanished.
REM
REM The library versions are checked by compile-app.cmd below, so there is no
REM second check here. A refusal still writes the sentinel, with the reason in
REM app-compile.log: the sentinel says the run ended, not that it compiled.
del /q C:\Users\Administrator\Documents\CAL\app-compile.log 2>nul
del /q C:\Users\Administrator\Documents\CAL\compile-done.flag 2>nul
call C:\Users\Administrator\Documents\CAL\compile-app.cmd
echo FINISHED > C:\Users\Administrator\Documents\CAL\compile-done.flag
