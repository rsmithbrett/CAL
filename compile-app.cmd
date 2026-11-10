@echo off
REM LovyanGFX and ArduinoJson come from the sketchbook, not from this
REM repository, so the versions are checked against the ones
REM ci/build-firmware.sh installs before anything is compiled against them.
call "%~dp0check-library-pins.cmd" "%~dp0app-compile.log"
if errorlevel 1 exit /b 1
cd /d C:\Users\Administrator\Documents\CAL\App
C:\Tools\arduino-cli\arduino-cli.exe compile --fqbn esp32:esp32:esp32:PartitionScheme=min_spiffs --libraries C:\Users\Administrator\Documents\CAL\libraries --export-binaries . > C:\Users\Administrator\Documents\CAL\app-compile.log 2>&1
echo EXIT=%ERRORLEVEL% >> C:\Users\Administrator\Documents\CAL\app-compile.log
