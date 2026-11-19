@echo off
chcp 65001 >nul
title SONAR SCAN  COM9
mode con: cols=110 lines=42 >nul 2>&1
cd /d "%~dp0"
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0扫描窗口.ps1"
echo.
echo [closed] press any key
pause >nul
