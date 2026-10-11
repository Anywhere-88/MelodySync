@echo off
REM ============================================================================
REM  MelodySync - static self-check before commit
REM
REM  Runs three checks; returns 0 only if all pass:
REM    1) tools\check_sources.py  source list / encoding audit
REM    2) tools\check_xml.py      vcxproj / filters / qrc well-formedness
REM    3) tools\check_js_scope.py inline JS scope guard (no-undef)
REM    4) tools\qr-verify\        (optional) QR encoder verification
REM
REM  ---------------------------------------------------------------------------
REM  WHY THIS FILE IS ASCII-ONLY - DO NOT ADD CHINESE HERE
REM
REM  cmd.exe mis-parses .bat files that contain non-ASCII bytes. Measured on this
REM  machine (Chinese Windows, CP936 console):
REM
REM    * a 6-line .bat whose only non-ASCII was one Chinese REM line still tried
REM      to RUN that comment as a command when the file used LF endings;
REM    * the UTF-8 + "chcp 65001" version of this very script made the batch
REM      reader lose its file position and re-execute lines FOREVER - ~900 KB of
REM      "is not recognized as an internal or external command" per 10 seconds,
REM      no child process, saturated CPU. The original check.bat and build.bat
REM      were both affected; build.bat ran "AT", dumped 2 MB of help text and
REM      never reached build.py at all.
REM
REM  Chinese output is NOT lost: it comes from the python scripts below, which
REM  print it themselves, and chcp 65001 below keeps the console able to show it.
REM  Only this file's own few lines are English.
REM  ---------------------------------------------------------------------------
REM
REM  NOT run by this script - run it by hand when touching the fallback path:
REM    python tools\probe_regression.py [-v] [--only <name>]
REM      Guards that a fallback download either matches the target duration
REM      (within 20%) or explicitly gives up - never a plain wrong version.
REM      Cases come from a real playlist. Depends on bilibili's current search
REM      results, so scores drift: it is a spot-check, NOT part of this gate.
REM
REM  Usage:
REM    check.bat           run 1) 2) 3)
REM    check.bat full      also run 4)
REM ============================================================================
setlocal EnableDelayedExpansion
REM chcp below is what lets the python children print Chinese without mojibake.
chcp 65001 >nul
cd /d "%~dp0"

set "PY="
REM Try in order: PYTHON env var -> python on PATH -> bundled WorkBuddy python
if defined PYTHON if exist "%PYTHON%" set "PY=%PYTHON%"
if not defined PY (
    where python >nul 2>nul
    if not errorlevel 1 set "PY=python"
)
if not defined PY (
    set "CAND=%USERPROFILE%\.workbuddy\binaries\python\versions\3.13.12\python.exe"
    if exist "!CAND!" set "PY=!CAND!"
)
if not defined PY (
    echo [ERROR] Python interpreter not found. Install Python 3.9+ or set PYTHON.
    exit /b 1
)
echo [MelodySync] Python: %PY%
echo.

set "RC=0"

echo ########## 1/3  source list / encoding audit ##########
"%PY%" tools\check_sources.py
if errorlevel 1 set "RC=1"
echo.

echo ########## 2/3  XML well-formedness / paths ##########
"%PY%" tools\check_xml.py
if errorlevel 1 set "RC=1"
echo.

echo ########## 3/3  JS scope guard (render-time ReferenceError) ##########
"%PY%" tools\check_js_scope.py
if errorlevel 1 set "RC=1"
echo.

if /i "%~1"=="full" (
    echo ########## 4/4  QR encoder verification ##########
    pushd tools\qr-verify
    "%PY%" compare2.py
    if errorlevel 1 set "RC=1"
    "%PY%" mask_audit.py
    if errorlevel 1 set "RC=1"
    popd
    echo.
)

if "%RC%"=="0" (
    echo ============================================================
    echo  ALL CHECKS PASSED
    echo ============================================================
) else (
    echo ============================================================
    echo  FAILURES FOUND - see FAIL lines above
    echo ============================================================
)
endlocal & exit /b %RC%
