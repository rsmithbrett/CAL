@echo off
REM Refuses a local compile when the installed LovyanGFX or ArduinoJson is not
REM the version ci/build-firmware.sh installs.
REM
REM The compile scripts pass --libraries libraries, which holds DamBoards and
REM nothing else. LovyanGFX and ArduinoJson resolve from the sketchbook, so a
REM local binary links whatever is installed there - the four FreeSansBold glyph
REM tables every width figure in App/ was measured off among them - and the
REM build output does not say which. CI installs exact versions, and this is
REM what makes a local binary comparable to a CI one.
REM
REM It reports and stops rather than installing. A compile that reinstalls a
REM library moves the toolchain under everything else built on this machine,
REM with no notice to whoever built it. The message carries the command to run.
REM
REM The pinned versions are read out of ci/build-firmware.sh, so that script
REM stays the one place they are written.
REM
REM Argument 1 is the log the calling script writes. A refusal goes there as
REM well as to the console, because a detached compile's console goes nowhere,
REM and it truncates that log first: the compile being refused never runs, so
REM what is in there belongs to the previous run and would read as this one's.

setlocal
set "ROOT=%~dp0"
if "%ROOT:~-1%"=="\" set "ROOT=%ROOT:~0,-1%"
set "PINFILE=%ROOT%\ci\build-firmware.sh"
set "PINLOG=%~1"
if not defined PINLOG set "PINLOG=NUL"
set "ARDUINOCLI=C:\Tools\arduino-cli\arduino-cli.exe"

if not exist "%PINFILE%" goto :no_pin_file

REM %%~v strips the quotes the shell variable is written with. Both this file
REM and library.properties come back without a line terminator, and findstr /x
REM is not used for the comparison because it matches a CRLF file and misses an
REM LF one, which is what an installed library.properties is.
set "WANT_LOVYANGFX="
set "WANT_ARDUINOJSON="
for /f "tokens=2 delims==" %%v in ('findstr /b /c:"LOVYANGFX_VERSION=" "%PINFILE%"') do set "WANT_LOVYANGFX=%%~v"
for /f "tokens=2 delims==" %%v in ('findstr /b /c:"ARDUINOJSON_VERSION=" "%PINFILE%"') do set "WANT_ARDUINOJSON=%%~v"
if not defined WANT_LOVYANGFX goto :no_pins
if not defined WANT_ARDUINOJSON goto :no_pins

REM The sketchbook is asked for rather than assumed, so a machine that keeps its
REM libraries somewhere else is read correctly instead of reported as empty.
set "SKETCHBOOK="
for /f "delims=" %%d in ('%ARDUINOCLI% config get directories.user') do if not defined SKETCHBOOK set "SKETCHBOOK=%%d"
if not defined SKETCHBOOK goto :no_sketchbook
set "LIBRARIES=%SKETCHBOOK%\libraries"

set "HAVE_LOVYANGFX=nothing"
set "HAVE_ARDUINOJSON=nothing"
if exist "%LIBRARIES%\LovyanGFX\library.properties" for /f "tokens=2 delims==" %%v in ('findstr /b /c:"version=" "%LIBRARIES%\LovyanGFX\library.properties"') do set "HAVE_LOVYANGFX=%%v"
if exist "%LIBRARIES%\ArduinoJson\library.properties" for /f "tokens=2 delims==" %%v in ('findstr /b /c:"version=" "%LIBRARIES%\ArduinoJson\library.properties"') do set "HAVE_ARDUINOJSON=%%v"

if not "%HAVE_LOVYANGFX%"=="%WANT_LOVYANGFX%" goto :mismatch
if not "%HAVE_ARDUINOJSON%"=="%WANT_ARDUINOJSON%" goto :mismatch

echo ==^> Libraries are the pinned ones: LovyanGFX %HAVE_LOVYANGFX%, ArduinoJson %HAVE_ARDUINOJSON%
exit /b 0

:mismatch
type nul > "%PINLOG%"
call :report "ERROR: the installed libraries are not the ones ci/build-firmware.sh installs."
call :report "       LovyanGFX   pinned %WANT_LOVYANGFX%, installed %HAVE_LOVYANGFX%"
call :report "       ArduinoJson pinned %WANT_ARDUINOJSON%, installed %HAVE_ARDUINOJSON%"
call :report "       Sketchbook  %LIBRARIES%"
call :report "       Install the pinned versions, then re-run this build:"
call :report "         %ARDUINOCLI% lib install LovyanGFX@%WANT_LOVYANGFX% ArduinoJson@%WANT_ARDUINOJSON%"
exit /b 1

:no_pin_file
type nul > "%PINLOG%"
call :report "ERROR: no %PINFILE%, so the pinned library versions cannot be read."
call :report "       The build is refused rather than run against whatever is installed."
exit /b 1

:no_pins
type nul > "%PINLOG%"
call :report "ERROR: %PINFILE% carries no LOVYANGFX_VERSION or ARDUINOJSON_VERSION line."
call :report "       The build is refused rather than run against whatever is installed."
exit /b 1

:no_sketchbook
type nul > "%PINLOG%"
call :report "ERROR: %ARDUINOCLI% config get directories.user returned nothing, so the"
call :report "       installed library versions cannot be read. Install arduino-cli or"
call :report "       point ARDUINOCLI in this script at it, then re-run."
exit /b 1

:report
echo %~1
echo %~1 >> "%PINLOG%"
exit /b 0
