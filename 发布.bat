@echo off
setlocal
chcp 437 >nul

rem ============================================================
rem  Release helper - one click publish
rem
rem  Edit VCODE / VNAME below, then double-click this file.
rem    1) builds the APK with that version
rem    2) writes dist\app.apk + dist\version.json
rem    3) commits and pushes to GitHub
rem
rem  After the push, phones get the update on next launch.
rem  No need to create a Release on the web page.
rem
rem  PROXY NOTE
rem    This machine has git http.proxy = 127.0.0.1:7890 which is
rem    usually NOT running, so a plain "git push" fails. The push
rem    below clears it and connects directly (verified working).
rem
rem  IMPORTANT
rem    repo must stay PUBLIC, otherwise both the app and every
rem    mirror get 404.
rem ============================================================

set VCODE=4
set VNAME=2.2
set VNOTE=auto-update support

rem  %~dp0 = the folder this .bat lives in.  Double-clicking a bat sets
rem  the working directory to System32, so we MUST cd to the script dir
rem  first, otherwise git runs outside the repo.
cd /d "%~dp0"

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
echo === pushing to GitHub ===
git add -A dist app/src app/AndroidManifest.xml app/build_apk.sh *.apk *.bat
git -c user.name="keepon365" -c user.email="keepon365@users.noreply.github.com" commit -m "release v%VCODE% %VNAME%"
if errorlevel 1 echo    (nothing new to commit, continuing)

set http_proxy=
set https_proxy=
set HTTP_PROXY=
set HTTPS_PROXY=
git -c http.proxy= -c https.proxy= push origin main
if errorlevel 1 goto pushfail

echo.
echo ============================================================
echo  DONE. Share this link with roommates:
echo.
echo  https://ghfast.top/https://raw.githubusercontent.com/keepon365/HEU_car/main/dist/app.apk
echo.
echo  In-app updater already points at:
echo  https://raw.githubusercontent.com/keepon365/HEU_car/main/dist/
echo ============================================================
echo.
pause
exit /b 0

:pushfail
echo.
echo [FAIL] git push failed. Check network / GitHub credentials.
pause
exit /b 1

:fail
echo.
echo [FAIL] build failed - read the messages above.
pause
exit /b 1
