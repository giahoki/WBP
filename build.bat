@echo off
rem Builds the Windows package in one step: the game runtime and the launcher (build.sh), then
rem the package and the zip (packaging\windows\package.sh).
rem
rem   build.bat            everything: dist\WBP\ and dist\WBP.zip
rem   build.bat launcher   only the launcher (Bloodborne.exe) and the package, after one full build
rem
rem Needs MSYS2 with the CLANG64 packages of packaging\windows\README.md (MSYS2_ROOT if it is
rem not in C:\msys64). No Python is needed.
setlocal
cd /d "%~dp0"
set "MSYS2=%MSYS2_ROOT%"
if "%MSYS2%"=="" set "MSYS2=C:\msys64"
if not exist "%MSYS2%\usr\bin\bash.exe" (
    echo MSYS2 was not found in %MSYS2%. Install it: winget install MSYS2.MSYS2
    echo then the packages listed in packaging\windows\README.md.
    goto :fail
)
set "MSYSTEM=CLANG64"
set "CHERE_INVOKING=1"
set "BASH=%MSYS2%\usr\bin\bash.exe"

if /i "%~1"=="launcher" (
    echo === Building the launcher ^(build.sh --launcher^) ===
    "%BASH%" -lc "bash build.sh --launcher" || goto :fail
) else (
    echo === Building the game runtime and the launcher ^(build.sh^) ===
    "%BASH%" -lc "bash build.sh" || goto :fail
)
echo === Packaging ^(packaging/windows/package.sh^) ===
"%BASH%" -lc "bash packaging/windows/package.sh" || goto :fail

echo.
echo Done: dist\WBP\Bloodborne.exe and dist\WBP.zip
exit /b 0

:fail
echo.
echo Build failed.
if not defined CI pause
exit /b 1
