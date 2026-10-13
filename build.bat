@echo off
rem Builds the Windows port (build.sh) and the package (packaging\windows\package.sh).
rem Started without arguments it asks what to build; with an argument it builds without asking:
rem
rem   build.bat all            the game, DLSS, the launcher and the package: dist\WBP\ and dist\WBP.zip
rem   build.bat dev            the game and the launcher in out\, no package (quick test build)
rem   build.bat game           only the game runtime (out\wbp-game.exe)
rem   build.bat launcher       only the launcher (Bloodborne.exe) and the package
rem   build.bat launcher-only  only the launcher, no package
rem   build.bat package        only the package, from what is already in out\
rem   build.bat pgo            an instrumented game (and DLSS) that records a PGO profile while played
rem
rem Needs MSYS2 with the CLANG64 packages of packaging\windows\README.md (MSYS2_ROOT if it is
rem not in C:\msys64). No Python is needed. DLSS (all, pgo) also needs Visual Studio with C++; the
rem DLSS SDK is fetched into third_party\DLSS-SDK the first time. Without them DLSS is skipped.
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

set "MODE=%~1"
set "ASKED="
if not "%MODE%"=="" goto :check
if defined CI set "MODE=all"
if defined CI goto :check

set "ASKED=1"
echo.
echo  ===== WBP build =====
echo.
echo   1  Everything: game + DLSS + launcher + package (dist\WBP and dist\WBP.zip)
echo   2  Game + launcher, no package (quick, to test from out\)
echo   3  Game only (wbp-game.exe)
echo   4  Launcher only (Bloodborne.exe) + package
echo   5  Launcher only, no package
echo   6  Package only, from what is already in out\
echo   7  Game that records a PGO profile (play 10-15 minutes, then build again)
echo   0  Exit
echo.
choice /c 12345670 /n /m "What to build? [1-7, 0]: "
set "PICK=%errorlevel%"
if "%PICK%"=="1" set "MODE=all"
if "%PICK%"=="2" set "MODE=dev"
if "%PICK%"=="3" set "MODE=game"
if "%PICK%"=="4" set "MODE=launcher"
if "%PICK%"=="5" set "MODE=launcher-only"
if "%PICK%"=="6" set "MODE=package"
if "%PICK%"=="7" set "MODE=pgo"
if "%PICK%"=="8" exit /b 0

:check
echo %MODE%| findstr /r /i /x "all dev game launcher launcher-only package pgo" >nul || (
    echo Unknown mode: %MODE%
    echo Modes: all, dev, game, launcher, launcher-only, package, pgo
    goto :fail
)
set "START=%time%"
goto :m_%MODE%

:m_all
call :build "" "game and launcher" || goto :fail
call :dlss
call :package || goto :fail
goto :done
:m_dev
call :build "" "game and launcher" || goto :fail
goto :done
:m_game
call :build "--game" "game" || goto :fail
goto :done
:m_launcher
call :build "--launcher" "launcher" || goto :fail
call :package || goto :fail
goto :done
:m_launcher-only
call :build "--launcher" "launcher" || goto :fail
goto :done
:m_package
call :package || goto :fail
goto :done
:m_pgo
set "BB_PGO=generate"
call :build "--game" "game recording a PGO profile" || goto :fail
call :dlss
goto :done

:done
echo.
echo Done ^(%START% - %time%^).
if /i "%MODE%"=="all"      echo Package: dist\WBP\Bloodborne.exe and dist\WBP.zip
if /i "%MODE%"=="launcher" echo Package: dist\WBP\Bloodborne.exe and dist\WBP.zip
if /i "%MODE%"=="dev"      echo Built in out\: wbp-game.exe, Bloodborne.exe
if /i "%MODE%"=="game"     echo Built out\wbp-game.exe
if /i "%MODE%"=="pgo"      echo Play this build: the profile goes to pgo\ every 30 s. Then build again ^(1 or 3^).
if defined ASKED pause
exit /b 0

:build
echo.
echo === Building: %~2 ^(build.sh %~1^) ===
"%BASH%" -lc "bash build.sh %~1"
exit /b %errorlevel%

:dlss
echo.
echo === DLSS ^(packaging/windows/build_dlss.sh^) ===
"%BASH%" -lc "bash packaging/windows/build_dlss.sh"
if errorlevel 1 echo DLSS was not built: the game works, the DLSS option stays unavailable.
exit /b 0

:package
echo.
echo === Packaging ^(packaging/windows/package.sh^) ===
"%BASH%" -lc "bash packaging/windows/package.sh"
exit /b %errorlevel%

:fail
echo.
echo Build failed.
if not defined CI pause
exit /b 1
