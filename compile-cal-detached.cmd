@echo off
del /q C:\Users\Administrator\Documents\CAL\cal-compile.log 2>nul
del /q C:\Users\Administrator\Documents\CAL\cal-done.flag 2>nul

REM LovyanGFX and ArduinoJson come from the sketchbook, not from this
REM repository, so the versions are checked against the ones
REM ci/build-firmware.sh installs before anything is compiled against them. The
REM sentinel is written on the way out as well, so a watcher waiting on it stops
REM waiting and reads the log.
call "%~dp0check-library-pins.cmd" "%~dp0cal-compile.log"
if errorlevel 1 (
  echo LIBRARY-PINS-FAILED > "%~dp0cal-done.flag"
  exit /b 1
)

cd /d C:\Users\Administrator\Documents\CAL
C:\Tools\arduino-cli\arduino-cli.exe compile --fqbn esp32:esp32:esp32:PartitionScheme=min_spiffs --libraries C:\Users\Administrator\Documents\CAL\libraries --export-binaries . > C:\Users\Administrator\Documents\CAL\cal-compile.log 2>&1
echo EXIT=%ERRORLEVEL% >> C:\Users\Administrator\Documents\CAL\cal-compile.log
echo FINISHED > C:\Users\Administrator\Documents\CAL\cal-done.flag
