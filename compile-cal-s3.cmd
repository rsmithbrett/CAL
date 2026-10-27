@echo off
REM Build CAL for the Panellan ZX2D80CE02S-V10 (ESP32-S3, 8MB, 2MB QSPI PSRAM).
REM
REM THE PARTITION TABLE IS SWAPPED IN AND OUT BY THIS SCRIPT. Arduino reads the
REM partitions.csv sitting beside the sketch and there is only one such name, so
REM selecting a board's table means putting it there. The 4MB table is restored
REM at the end whether the build worked or not, and the restore is checked.
REM
REM If this script is interrupted between the swap and the restore, the working
REM tree is left holding the 8MB table under the name partitions.csv. That is
REM why the first thing below refuses to start when partitions.csv already
REM differs from the committed one: a second run would otherwise back up the
REM WRONG table and make the mix-up permanent.
REM
REM FQBN notes:
REM   PSRAM=enabled   QSPI, settled by measurement - the module is a WROVER-N8R2
REM                   and the installed S3 libraries are CONFIG_SPIRAM_MODE_QUAD.
REM   CDCOnBoot=cdc   The S3 has native USB. Without this, Serial goes nowhere
REM                   and a running board looks dead.
REM   PartitionScheme=custom
REM                   Declares build.partitions empty and lifts
REM                   upload.maximum_size to 16777216, so the sketch's own table
REM                   is used and the min_spiffs ceiling of 1966080 does not
REM                   refuse a valid build.
REM   build.flash_size=8MB
REM                   The generic esp32s3 board has NO FlashSize menu and hard
REM                   defaults to 4MB. Left at 4MB the image header disagrees
REM                   with an 8MB table and partitions past 4MB are rejected.

setlocal
set ROOT=C:\Users\Administrator\Documents\CAL
cd /d %ROOT%

echo ==^> Checking the working tree's partition table is the committed one
git diff --quiet -- partitions.csv
if errorlevel 1 (
  echo ERROR: partitions.csv differs from HEAD.
  echo        A previous run may have been interrupted before restoring it.
  echo        Inspect it, restore the 4MB table, then re-run.
  exit /b 1
)

del /q "%ROOT%\cal-s3-compile.log" 2>nul
del /q "%ROOT%\cal-s3-done.flag" 2>nul

echo ==^> Swapping in the 8MB table
copy /y "%ROOT%\partitions.csv" "%ROOT%\partitions.csv.4mb-backup" >nul
copy /y "%ROOT%\partitions-panellan-8mb.csv" "%ROOT%\partitions.csv" >nul

echo ==^> Compiling
C:\Tools\arduino-cli\arduino-cli.exe compile ^
  --fqbn esp32:esp32:esp32s3:PSRAM=enabled,CDCOnBoot=cdc,PartitionScheme=custom ^
  --build-property build.flash_size=8MB ^
  --build-path "%ROOT%\build-s3" ^
  --export-binaries . > "%ROOT%\cal-s3-compile.log" 2>&1
set BUILD_RC=%ERRORLEVEL%
echo EXIT=%BUILD_RC% >> "%ROOT%\cal-s3-compile.log"

echo ==^> Restoring the 4MB table
copy /y "%ROOT%\partitions.csv.4mb-backup" "%ROOT%\partitions.csv" >nul
del /q "%ROOT%\partitions.csv.4mb-backup" 2>nul

REM Verified against git rather than against the backup, so a corrupt backup
REM cannot pass this.
git diff --quiet -- partitions.csv
if errorlevel 1 (
  echo RESTORE FAILED: partitions.csv still differs from HEAD. >> "%ROOT%\cal-s3-compile.log"
  echo RESTORE FAILED - fix partitions.csv before any other build.
  echo RESTORE-FAILED > "%ROOT%\cal-s3-done.flag"
  exit /b 1
)
echo restore verified >> "%ROOT%\cal-s3-compile.log"

echo FINISHED > "%ROOT%\cal-s3-done.flag"
exit /b %BUILD_RC%
