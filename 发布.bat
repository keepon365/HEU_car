@echo off
setlocal
chcp 437 >nul

rem ============================================================
rem  Release helper for HEU_car remote
rem
rem  Edit VCODE / VNAME below, then double-click this file.
rem    1) builds the APK with that version
rem    2) writes dist\app.apk + dist\version.json
rem    3) prints the Release upload URL
rem
rem  Then upload these TWO files as Release assets:
rem      dist\app.apk
rem      dist\version.json
rem
rem  IMPORTANT
rem    - repo must be PUBLIC. private repo => 404 for the app and
rem      for every mirror (they have no token).
rem    - do NOT tick "pre-release": releases/latest/download only
rem      follows the newest NON-prerelease release.
rem    - asset names must stay exactly app.apk / version.json
rem ============================================================

set VCODE=4
set VNAME=2.2
set VNOTE=auto-update support

set ROOT=%CD:\=/%
set BASH=C:\Program Files\Git\bin\bash.exe
if not exist "%BASH%" set BASH=bash

echo.
echo   versionCode : %VCODE%
echo   versionName : %VNAME%
echo   note        : %VNOTE%
echo.

"%BASH%" -c "cd '%ROOT%' && VCODE=%VCODE% VNAME=%VNAME% NOTE='%VNOTE%' bash app/build_apk.sh"
if errorlevel 1 goto fail

echo.
echo ============================================================
echo  Upload these two files to a NEW Release:
echo     dist\app.apk
echo     dist\version.json
echo.
echo  Tag name suggestion : v%VCODE%
echo  Release page        : https://github.com/keepon365/HEU_car/releases/new
echo ============================================================
echo.
pause
exit /b 0

:fail
echo.
echo [FAIL] build failed - read the messages above.
pause
exit /b 1
