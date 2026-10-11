@echo off
del /q C:\Users\Administrator\Documents\CAL\cal-compile.log 2>nul
del /q C:\Users\Administrator\Documents\CAL\cal-done.flag 2>nul
cd /d C:\Users\Administrator\Documents\CAL
C:\Tools\arduino-cli\arduino-cli.exe compile --fqbn esp32:esp32:esp32:PartitionScheme=min_spiffs --export-binaries . > C:\Users\Administrator\Documents\CAL\cal-compile.log 2>&1
echo EXIT=%ERRORLEVEL% >> C:\Users\Administrator\Documents\CAL\cal-compile.log
echo FINISHED > C:\Users\Administrator\Documents\CAL\cal-done.flag
