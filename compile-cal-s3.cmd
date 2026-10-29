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
REM                   refuse a valid build. A 16MB ceiling accepts any image this
REM                   board can produce, so arduino-cli will not refuse one that
REM                   overflows a partition - the size check at the end of this
REM                   script is what holds the image to its own table.
REM
REM Output goes to the build directory arduino-cli names after the FQBN, so the
REM two boards do not clobber each other and .gitignore already covers both.
REM
REM   build.flash_size=8MB
REM                   The generic esp32s3 board has NO FlashSize menu and hard
REM                   defaults to 4MB. Left at 4MB the image header disagrees
REM                   with an 8MB table and partitions past 4MB are rejected.
REM
REM --libraries %ROOT%\libraries puts the in-repo DamBoards library on the search
REM path, which is where BoardPanellan.h lives now that CAL and App read one copy
REM of it. It is passed rather than relying on a copy under the sketchbook's own
REM libraries folder, because a library that exists only on this workstation is
REM not in version control and is not on a CI machine - the build would work here
REM and fail there. The flag ADDS to the default search path, so LovyanGFX and
REM ArduinoJson still resolve from the sketchbook as before.

setlocal
REM The tree this script builds is the tree it sits in. %~dp0 names the directory
REM holding this file, with a trailing backslash that the next line takes off.
REM Deriving it rather than writing a path out is what lets a worktree or a second
REM checkout build itself: a written-out path builds that one tree and size-checks
REM that one tree's binary, from wherever the script is run.
set ROOT=%~dp0
if "%ROOT:~-1%"=="\" set ROOT=%ROOT:~0,-1%
REM The same path with forward slashes, for arguments handed to bash: dirname
REM and the rest of the check script read a backslash as text, not a separator.
REM Derived from ROOT rather than written again, so the two cannot name
REM different trees.
set ROOTFS=%ROOT:\=/%
set GITBASH=C:\Program Files\Git\bin\bash.exe
set S3BUILD=build/esp32.esp32.esp32s3
cd /d "%ROOT%"

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
  --libraries "%ROOT%\libraries" ^
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

REM The size check measures a binary from this run, so a failed compile reports
REM that failure instead and leaves the previous run's image alone.
if not "%BUILD_RC%"=="0" goto :compile_failed

REM ==> SIZE CHECK. ci/check-partition-fit.sh compares the image against the
REM table in its own build directory, which is the 8MB table this build used -
REM arduino-cli copies it there, so the check reads the right ceiling even
REM though the swap above has already been undone by the time it runs.
REM
REM factory is where CAL runs. ota_0 is where a CAL update is staged and booted
REM before it copies itself into factory, so a CAL image has to fit both (see
REM CAL_OTA_DESIGN.md section 5.4). The pre-re-table field ceiling in
REM ci/build-firmware.sh is not checked here: that number is the layout on the
REM deployed 4MB units and says nothing about this board.
REM
REM A missing bash is a failure rather than a skip. An unchecked image reads the
REM same as a checked one in a log, and this board has no other ceiling on it.
echo ==^> Verifying compiled size against the table this build used
if not exist "%GITBASH%" goto :no_bash

"%GITBASH%" "%ROOTFS%/ci/check-partition-fit.sh" "%ROOTFS%/%S3BUILD%/CAL.ino.bin" factory "CAL Panellan (factory)" >> "%ROOT%\cal-s3-compile.log" 2>&1
if errorlevel 1 goto :size_check_failed

"%GITBASH%" "%ROOTFS%/ci/check-partition-fit.sh" "%ROOTFS%/%S3BUILD%/CAL.ino.bin" ota_0 "CAL Panellan (must also fit ota_0, for OTA staging)" >> "%ROOT%\cal-s3-compile.log" 2>&1
if errorlevel 1 goto :size_check_failed

echo FINISHED > "%ROOT%\cal-s3-done.flag"
exit /b 0

:size_check_failed
echo SIZE CHECK FAILED - see cal-s3-compile.log.
echo SIZE-CHECK-FAILED > "%ROOT%\cal-s3-done.flag"
exit /b 1

:no_bash
echo ERROR: no bash at "%GITBASH%" - the size check cannot run, and this image has
echo        no other ceiling on it. Install Git for Windows or point GITBASH at a
echo        bash, then re-run.
echo SIZE-CHECK-UNAVAILABLE > "%ROOT%\cal-s3-done.flag"
exit /b 1

:compile_failed
echo FINISHED > "%ROOT%\cal-s3-done.flag"
exit /b %BUILD_RC%
