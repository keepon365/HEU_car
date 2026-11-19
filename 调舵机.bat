@echo off
chcp 65001 >nul
title SERVO  COM9  auto-reconnect
mode con: cols=100 lines=40 >nul 2>&1
cd /d "%~dp0"
set "CLI=%SystemDrive%\Program Files\Arduino IDE\resources\app\lib\backend\resources\arduino-cli.exe"
set /a N=0

:loop
set /a N+=1
echo.
echo ==== connect attempt #%N%   %date% %time% ====
"%CLI%" monitor -p COM9 -c baudrate=115200 -c dtr=off -c rts=off
echo.
echo ==== link down, retrying in 1s ====
ping -n 2 127.0.0.1 >nul
goto loop
