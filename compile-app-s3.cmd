@echo off
REM Build the App for the Panellan ZX2D80CE02S-V10 (ESP32-S3, 8MB, 2MB QSPI
REM PSRAM). The CAL half of this is compile-cal-s3.cmd and the two scripts are
REM deliberately the same shape.
REM
REM THE PARTITION TABLE IS SWAPPED IN AND OUT BY THIS SCRIPT. Arduino reads the
REM partitions.csv sitting beside the sketch and there is only one such name, so
REM selecting a board's table means putting it there. The 4MB table is restored
REM at the end whether the build worked or not, and the restore is checked.
REM
REM If this script is interrupted between the swap and the restore, the working
REM tree is left holding the 8MB table under the name App\partitions.csv. That
REM is why the first thing below refuses to start when App\partitions.csv
REM already differs from the committed one: a second run would otherwise back up
REM the WRONG table and make the mix-up permanent.
REM
REM A left-behind 8MB App\partitions.csv also breaks ci/build-firmware.sh's
REM table comparison, which diffs it against the root 4MB table byte-for-byte.
REM That check firing is the backstop for this script's restore failing.
REM
REM FQBN notes, identical to CAL's:
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
REM
REM --libraries %ROOT%\libraries puts the in-repo DamBoards library on the search
REM path, which is where BoardPanellan.h lives. It is passed rather than relying
REM on a copy under the sketchbook's own libraries folder, because a library that
REM exists only on this workstation is not in version control and is not on a CI
REM machine - the build would work here and fail there. The flag ADDS to the
REM default search path, so LovyanGFX and ArduinoJson still resolve from the
REM sketchbook as before.
REM
REM Output goes to the build directory arduino-cli names after the FQBN, so the
REM two boards do not clobber each other and .gitignore already covers both.

setlocal
REM The tree this script builds is the tree it sits in. %~dp0 names the directory
REM holding this file, with a trailing backslash that the next line takes off so
REM %ROOT%\App does not come out with a doubled separator. Deriving it rather than
REM writing a path out is what lets a worktree or a second checkout build itself:
REM a written-out path builds that one tree and size-checks that one tree's
REM binary, from wherever the script is run.
set ROOT=%~dp0
if "%ROOT:~-1%"=="\" set ROOT=%ROOT:~0,-1%
REM The same path with forward slashes, for arguments handed to bash: dirname
REM and the rest of the check script read a backslash as text, not a separator.
REM Derived from ROOT rather than written again, so the two cannot name
REM different trees.
set ROOTFS=%ROOT:\=/%
set GITBASH=C:\Program Files\Git\bin\bash.exe
set S3BUILD=App/build/esp32.esp32.esp32s3
cd /d "%ROOT%"

echo ==^> Checking the working tree's App partition table is the committed one
git diff --quiet -- App/partitions.csv
if errorlevel 1 (
  echo ERROR: App\partitions.csv differs from HEAD.
  echo        A previous run may have been interrupted before restoring it.
  echo        Inspect it, restore the 4MB table, then re-run.
  exit /b 1
)

del /q "%ROOT%\app-s3-compile.log" 2>nul
del /q "%ROOT%\app-s3-done.flag" 2>nul

REM Copied from App's own partitions-panellan-8mb.csv, beside this sketch, rather
REM than from the root copy, so this build uses the table that sits with the
REM sketch it bakes into App.ino.merged.bin. The two are byte-identical, and
REM `diff partitions-panellan-8mb.csv App/partitions-panellan-8mb.csv` is how to
REM confirm it. Nothing automates that check.
REM
REM Nothing in ci/ pairs the 8MB tables. ci/build-firmware.sh diffs exactly one
REM pair, root partitions.csv against App/partitions.csv, which is the 4MB pair.
REM There is no loop pairing tables by filename and no reporting of an unpaired
REM table anywhere in ci/, so a drifted App-side 8MB table is caught by the hand
REM diff above or not at all.
REM
REM That matters because App.ino.merged.bin is a published release asset and bakes
REM its table in at 0x8000. An App-side table that has drifted re-tables any
REM device flashed with that image without saying so, which is the failure the 4MB
REM pair was caught in on 2026-09-15.
echo ==^> Swapping in the 8MB table
copy /y "%ROOT%\App\partitions.csv" "%ROOT%\App\partitions.csv.4mb-backup" >nul
copy /y "%ROOT%\App\partitions-panellan-8mb.csv" "%ROOT%\App\partitions.csv" >nul

echo ==^> Compiling
cd /d "%ROOT%\App"
C:\Tools\arduino-cli\arduino-cli.exe compile ^
  --fqbn esp32:esp32:esp32s3:PSRAM=enabled,CDCOnBoot=cdc,PartitionScheme=custom ^
  --build-property build.flash_size=8MB ^
  --libraries "%ROOT%\libraries" ^
  --export-binaries . > "%ROOT%\app-s3-compile.log" 2>&1
set BUILD_RC=%ERRORLEVEL%
echo EXIT=%BUILD_RC% >> "%ROOT%\app-s3-compile.log"
cd /d "%ROOT%"

echo ==^> Restoring the 4MB table
copy /y "%ROOT%\App\partitions.csv.4mb-backup" "%ROOT%\App\partitions.csv" >nul
del /q "%ROOT%\App\partitions.csv.4mb-backup" 2>nul

REM Verified against git rather than against the backup, so a corrupt backup
REM cannot pass this.
git diff --quiet -- App/partitions.csv
if errorlevel 1 (
  echo RESTORE FAILED: App/partitions.csv still differs from HEAD. >> "%ROOT%\app-s3-compile.log"
  echo RESTORE FAILED - fix App\partitions.csv before any other build.
  echo RESTORE-FAILED > "%ROOT%\app-s3-done.flag"
  exit /b 1
)
echo restore verified >> "%ROOT%\app-s3-compile.log"

REM The size check measures a binary from this run, so a failed compile reports
REM that failure instead and leaves the previous run's image alone.
if not "%BUILD_RC%"=="0" goto :compile_failed

REM ==> SIZE CHECK. ci/check-partition-fit.sh compares the image against the
REM table in its own build directory, which is the 8MB table this build used -
REM arduino-cli copies it there, so the check reads the right ceiling even
REM though the swap above has already been undone by the time it runs.
REM
REM ota_0 only: the App is delivered over the air into ota_0 and runs there,
REM and factory holds CAL. The pre-re-table field ceiling in
REM ci/build-firmware.sh is not checked here either - that number is the layout
REM on the deployed 4MB units and says nothing about this board.
REM
REM A missing bash is a failure rather than a skip. An unchecked image reads the
REM same as a checked one in a log, and PartitionScheme=custom means
REM arduino-cli will not refuse an oversized one.
echo ==^> Verifying compiled size against the table this build used
if not exist "%GITBASH%" goto :no_bash

"%GITBASH%" "%ROOTFS%/ci/check-partition-fit.sh" "%ROOTFS%/%S3BUILD%/App.ino.bin" ota_0 "App Panellan (ota_0)" >> "%ROOT%\app-s3-compile.log" 2>&1
if errorlevel 1 goto :size_check_failed

echo FINISHED > "%ROOT%\app-s3-done.flag"
exit /b 0

:size_check_failed
echo SIZE CHECK FAILED - see app-s3-compile.log.
echo SIZE-CHECK-FAILED > "%ROOT%\app-s3-done.flag"
exit /b 1

:no_bash
echo ERROR: no bash at "%GITBASH%" - the size check cannot run, and this image has
echo        no other ceiling on it. Install Git for Windows or point GITBASH at a
echo        bash, then re-run.
echo SIZE-CHECK-UNAVAILABLE > "%ROOT%\app-s3-done.flag"
exit /b 1

:compile_failed
echo FINISHED > "%ROOT%\app-s3-done.flag"
exit /b %BUILD_RC%
