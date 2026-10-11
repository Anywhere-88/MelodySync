@echo off
REM ============================================================================
REM  MelodySync - one-click build script (Windows)
REM
REM  Usage:
REM    build.bat              configure + build Release (default) -> build\bin\MelodySync.exe
REM    build.bat debug        configure + build Debug            -> build-debug\bin\MelodySync.exe
REM    build.bat reconfigure  wipe the build dir and reconfigure (after editing CMakeLists/vcpkg.json)
REM    build.bat run          build, then launch in --dev mode (reads ui/index.html from disk)
REM    build.bat clean        delete the build directories
REM    build.bat msbuild      use the hand-written MelodySync.sln + MSBuild (no CMake, fallback path)
REM
REM  Prerequisites (the script checks these and tells you what is missing):
REM    * Python 3.8+ (standard library only, any 3.x works)
REM    * Qt 6.5.x msvc2019_64 (with WebEngine / WebChannel / **Positioning**)
REM    * vcpkg (D:\OI\AI\AIWork\Music\vcpkg) - dependencies already installed, see README section 2.2
REM    * prebuilt FFmpeg under D:\OI\AI\AIWork\Music\deps\, see README section 2.7
REM
REM  ---------------------------------------------------------------------------
REM  WHY THIS SCRIPT IS SO THIN
REM
REM  It does one thing: find Python, then hand the arguments to tools\build.py.
REM
REM  The real build logic lives in build.py because this machine's toolchain has
REM  THREE hard constraints and getting any of them wrong fails the build
REM  (see README section 2.6):
REM      MSVC must be 14.44 (14.51 removed stdext::checked_array_iterator, which Qt 6.5 uses)
REM      CMake must be 3.30 (4.x fails in find_package(Qt6 ... WebEngineWidgets))
REM      generator must be Ninja (the VS generator reports
REM                              "No CMAKE_CXX_COMPILER could be found" here)
REM  Add INCLUDE/LIB/PATH needing vcvars64 and an explicit VCPKG_INSTALLED_DIR, and
REM  a batch implementation becomes unmaintainable while a Python one stays obvious.
REM
REM  ---------------------------------------------------------------------------
REM  WHY THIS FILE IS ASCII-ONLY - DO NOT ADD CHINESE HERE
REM
REM  cmd.exe mis-parses .bat files containing non-ASCII bytes. Measured here:
REM    * the UTF-8 + "chcp 65001" version made the batch reader lose its file
REM      position and re-execute lines: this script ran the AT command, dumped
REM      about 2 MB of AT help text and NEVER reached build.py;
REM    * check.bat did the same and looped forever (~900 KB of
REM      "is not recognized" per 10 seconds, no child process, saturated CPU).
REM  Both were broken from the start, which is why this project has always been
REM  built with "python tools\build.py" by hand.
REM
REM  Chinese output is not lost: tools\build.py prints it, and chcp 65001 below
REM  keeps the console able to display it. Only this file's own lines are English.
REM ============================================================================
setlocal EnableDelayedExpansion

REM chcp below is what lets the python child print Chinese without mojibake.
chcp 65001 >nul

cd /d "%~dp0"

set "ACTION=%~1"
if "%ACTION%"=="" set "ACTION=release"

REM ---------- clean: no Python needed, done right here ----------
if /i "%ACTION%"=="clean" (
    echo [MelodySync] removing build\ and build-debug\ ...
    if exist "build"       rmdir /s /q "build"
    if exist "build-debug" rmdir /s /q "build-debug"
    echo [MelodySync] done.
    goto :eof
)

REM ---------- locate Python ----------
REM Prefer python on PATH; fall back to the bundled WorkBuddy interpreter.
set "PY="
where python >nul 2>nul && set "PY=python"
if not defined PY if exist "%USERPROFILE%\.workbuddy\binaries\python\versions\3.13.12\python.exe" set "PY=%USERPROFILE%\.workbuddy\binaries\python\versions\3.13.12\python.exe"
if not defined PY (
    echo [ERROR] Python not found.
    echo         tools\build.py needs Python 3.8+ ^(standard library only, no packages^).
    echo         After installing, make sure python is on PATH, or just run:
    echo             python tools\build.py
    goto :fail
)

if not exist "tools\build.py" (
    echo [ERROR] tools\build.py is missing - it is the actual build script.
    goto :fail
)

REM ---------- msbuild branch: hand-written sln (fallback, not verified here) ----------
REM Requires the "Qt Visual Studio Tools" extension with Qt Versions configured,
REM otherwise the QtMsBuild props/targets are missing and it fails outright.
REM See README section 4.
if /i "%ACTION%"=="msbuild" (
    call :find_vs
    if errorlevel 1 goto :fail
    echo [MelodySync] building MelodySync.sln with MSBuild ^(Release^|x64^) ...
    echo [MelodySync] note: this path needs the VS Qt extension and is NOT verified here ^(README section 4^)
    "%MSBUILD%" MelodySync.sln /m /p:Configuration=Release /p:Platform=x64
    if errorlevel 1 goto :fail
    echo.
    echo [MelodySync] output: MelodySync\build\bin\MelodySync.exe
    goto :eof
)

REM ---------- normal branch: forward to tools\build.py ----------
set "PYARGS="
if /i "%ACTION%"=="debug"       set "PYARGS=--debug"
if /i "%ACTION%"=="reconfigure" set "PYARGS=--clean"
if /i "%ACTION%"=="run"         set "PYARGS=--run"
if /i "%ACTION%"=="release"     set "PYARGS="
if not defined PYARGS if /i not "%ACTION%"=="release" (
    echo [ERROR] unknown action: %ACTION%
    echo         available: release ^(default^) / debug / reconfigure / run / clean / msbuild
    goto :fail
)

"%PY%" "tools\build.py" !PYARGS!
if errorlevel 1 goto :fail

echo.
echo ============================================================
if /i "%ACTION%"=="debug" (
    echo  BUILD OK ^(Debug^)
    echo    exe     : build-debug\bin\MelodySync.exe
) else (
    echo  BUILD OK
    echo    exe     : build\bin\MelodySync.exe
)
echo    frontend: embedded in the exe via qrc ^(resource: /ui/index.html^)
echo    run     : build.bat run    or double-click the desktop shortcut
echo ============================================================
goto :eof


REM ==========================================================================
REM  Subroutine: locate VS MSBuild (only the msbuild branch uses this)
REM ==========================================================================
:find_vs
if defined MSBUILD if exist "%MSBUILD%" exit /b 0
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if exist "%VSWHERE%" (
    for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.Component.MSBuild -property installationPath`) do (
        set "VSPATH=%%i"
    )
    if defined VSPATH (
        set "MSBUILD=!VSPATH!\MSBuild\Current\Bin\MSBuild.exe"
        if exist "!MSBUILD!" (
            echo [MelodySync] MSBuild : !MSBUILD!
            exit /b 0
        )
    )
)
REM Fall back to msbuild on PATH (the case inside a Developer Command Prompt)
where msbuild >nul 2>nul
if not errorlevel 1 (
    set "MSBUILD=msbuild"
    echo [MelodySync] MSBuild: using msbuild from PATH
    exit /b 0
)
echo [ERROR] MSBuild not found. Install the "Desktop development with C++" workload.
exit /b 1

:fail
echo.
echo [MelodySync] BUILD FAILED - see the errors above.
endlocal
exit /b 1
