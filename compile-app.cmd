@echo off
cd /d C:\Users\Administrator\Documents\CAL\App
C:\Tools\arduino-cli\arduino-cli.exe compile --fqbn esp32:esp32:esp32:PartitionScheme=min_spiffs --export-binaries . > C:\Users\Administrator\Documents\CAL\app-compile.log 2>&1
echo EXIT=%ERRORLEVEL% >> C:\Users\Administrator\Documents\CAL\app-compile.log
