@echo off
chcp 936 >nul
setlocal
title HEU_car  param + flash

rem ============================================================
rem   EDIT ONLY THIS BLOCK, save, then double-click this file.
rem   These numbers are written into DirSpeedTest.ino and flashed.
rem
rem   WHEEL GAIN : 256 = no trim.  Smaller number = that wheel slower.
rem   car drifts LEFT  -> right wheel too fast -> GAIN_R smaller (try 246)
rem   car drifts RIGHT -> left  wheel too fast -> GAIN_L smaller
rem   move 2..4 at a time, never jump 10.
rem
rem   TRIM : extra duty so a wheel starts from standstill.
rem   DUTY : 0..255 for forward / backward / spin-in-place.
rem ============================================================
set GAIN_L=256
set GAIN_R=225
set TRIM_L=13
set TRIM_R=0
set DUTY_BASE=120
set DUTY_BACK=96
set DUTY_SPIN=108
set DUTY_TURN=65
set SPEED=1200
set RAMP_START=40
set RAMP_MS=2500
rem   RAMP_START : speed when you just pressed, in percent of the cap.
rem                65 = start at 65%% of DUTY_BASE, climb to 100%%. below 60
rem                the left wheel may not start at all (its threshold is 73).
rem   RAMP_MS    : how long you must hold to reach the cap. 1600 = like a
rem                gamepad trigger, 2500 = very linear, 800 = almost instant.
rem                the cap itself is DUTY_BASE (times SPEED).
rem   DUTY_TURN : outer wheel only, used by << and >> (left / right turn)
rem               smaller = each tap turns a smaller angle. min about 80.
rem   SPEED     : 1000 = 100.0%%. 975 = 97.5%%. 1050 = 105%%. 600..1400
rem               scales forward / back / turn / spin, but NOT the start kick.
rem ============================================================

set "SKETCH=%~dp0DirSpeedTest.ino"
set "FQBN=arduino:avr:uno"
set "BUILD=%TEMP%\heu_car_build"
set "CLI=C:\Program Files\Arduino IDE\resources\app\lib\backend\resources\arduino-cli.exe"

echo.
echo ==============================================================
echo   HEU_car  ^|  DirSpeedTest  param + flash
echo ==============================================================
echo   GAIN   L=%GAIN_L%   R=%GAIN_R%
echo   TRIM   L=%TRIM_L%   R=%TRIM_R%
echo   DUTY   FWD=%DUTY_BASE%   BACK=%DUTY_BACK%   SPIN=%DUTY_SPIN%   TURN=%DUTY_TURN%
echo   SPEED  %SPEED%   (1000 = 100.0%%)
echo   RAMP   start %RAMP_START%%% of cap, full at %RAMP_MS% ms (cap = DUTY_BASE)
echo --------------------------------------------------------------

if not exist "%SKETCH%" goto nosketch
if not exist "%CLI%" for %%c in (arduino-cli.exe) do set "CLI=%%~$PATH:c"
if not exist "%CLI%" goto nocli

echo [1/5] patch DirSpeedTest.ino ...
powershell -NoProfile -ExecutionPolicy Bypass -Command "$p='%SKETCH%'; $s=[IO.File]::ReadAllText($p,[Text.Encoding]::UTF8); $m=@(@('GAIN_LEFT',$env:GAIN_L),@('GAIN_RIGHT',$env:GAIN_R),@('TRIM_LEFT',$env:TRIM_L),@('TRIM_RIGHT',$env:TRIM_R),@('DUTY_BASE',$env:DUTY_BASE),@('DUTY_BACK',$env:DUTY_BACK),@('DUTY_SPIN',$env:DUTY_SPIN),@('DUTY_TURN',$env:DUTY_TURN),@('SPEED_PERMILLE',$env:SPEED),@('RAMP_START_PCT',$env:RAMP_START),@('RAMP_FULL_MS',$env:RAMP_MS)); foreach($x in $m){ $s=[regex]::Replace($s,'(?m)^(#define\s+'+$x[0]+'\s+)\d+','${1}'+$x[1]) }; [IO.File]::WriteAllText($p,$s,(New-Object System.Text.UTF8Encoding($false)))"
if errorlevel 1 goto patchfail

powershell -NoProfile -ExecutionPolicy Bypass -Command "$s=[IO.File]::ReadAllText('%SKETCH%',[Text.Encoding]::UTF8); foreach($n in 'GAIN_LEFT','GAIN_RIGHT','TRIM_LEFT','TRIM_RIGHT','DUTY_BASE','DUTY_BACK','DUTY_SPIN','DUTY_TURN','SPEED_PERMILLE','RAMP_START_PCT','RAMP_FULL_MS'){ $m=[regex]::Match($s,'(?m)^#define\s+'+$n+'\s+(\d+)'); if($m.Success){ Write-Host ('   '+$n.PadRight(14)+'= '+$m.Groups[1].Value) } }"

echo.
echo [2/5] find board ...
set "PORT="
"%CLI%" board list > "%TEMP%\hc_ports.txt" 2>nul
for /f "tokens=1" %%p in ('findstr /b /c:"COM" "%TEMP%\hc_ports.txt"') do set "PORT=%%p"
if "%PORT%"=="" goto noport
echo        port = %PORT%

echo.
echo [3/5] compile ...
if not exist "%BUILD%\DirSpeedTest" mkdir "%BUILD%\DirSpeedTest"
copy /y "%SKETCH%" "%BUILD%\DirSpeedTest\DirSpeedTest.ino" >nul
"%CLI%" compile --fqbn %FQBN% --build-path "%BUILD%" "%BUILD%\DirSpeedTest"
if errorlevel 1 goto buildfail

echo.
echo [4/5] upload to %PORT% ...
"%CLI%" upload -p %PORT% --fqbn %FQBN% --build-path "%BUILD%" "%BUILD%\DirSpeedTest"
if errorlevel 1 goto uploadfail

echo.
echo [5/5] read back ...
powershell -NoProfile -ExecutionPolicy Bypass -Command "try{ $sp=New-Object System.IO.Ports.SerialPort('%PORT%',9600,'None',8,'One'); $sp.Encoding=[Text.Encoding]::UTF8; $sp.DtrEnable=$true; $sp.Open(); Start-Sleep -Milliseconds 2600; $b=$sp.ReadExisting(); $sp.Write([char]68+[char]10); Start-Sleep -Milliseconds 400; $sp.Write([char]63+[char]10); Start-Sleep -Milliseconds 500; $r=$sp.ReadExisting(); $sp.Close(); Write-Host ''; Write-Host '--- board banner ---'; Write-Host $b.Trim(); Write-Host '--- values after clearing EEPROM ---'; Write-Host $r.Trim(); Write-Host ''; if($r -match '\[CAL\] L=(\d+) R=(\d+) S=(\d+)'){ if(($matches[1] -eq $env:GAIN_L) -and ($matches[2] -eq $env:GAIN_R) -and ($matches[3] -eq $env:SPEED)){ Write-Host '[OK] readback matches the numbers above' } else { Write-Host ('[WARN] readback L='+$matches[1]+' R='+$matches[2]+' S='+$matches[3]+'  but setting L='+$env:GAIN_L+' R='+$env:GAIN_R+' S='+$env:SPEED) } } else { Write-Host '[NOTE] no [CAL] line found, check the banner by eye' } }catch{ Write-Host ('[NOTE] cannot open port (close the IDE serial monitor): '+$_.Exception.Message) }"

echo.
echo ==============================================================
echo  DONE. Before you drive:
echo   - EEPROM was cleared, the values above are what runs now
echo   - wheels off the ground first, check both sides turn and no noise
echo   - 2 m straight run, measure the side offset, then retune GAIN_R
echo.
echo  Tune without flashing: serial monitor 9600, send  R246  L252  ?  W
echo ==============================================================
goto done

:nosketch
echo [ERROR] DirSpeedTest.ino not found next to this file.
goto fail

:nocli
echo [ERROR] arduino-cli.exe not found.
echo         Default: C:\Program Files\Arduino IDE\resources\app\lib\backend\resources\arduino-cli.exe
echo         If the IDE lives elsewhere, edit the CLI line in this file.
goto fail

:noport
echo [ERROR] no serial port found. Check USB cable, PWR led,
echo         and "USB-SERIAL CH340" in Device Manager.
goto fail

:patchfail
echo [ERROR] patch failed. Is DirSpeedTest.ino locked by the IDE?
goto fail

:buildfail
echo [ERROR] compile failed, see the first "error" line above.
goto fail

:uploadfail
echo [ERROR] upload failed. The usual cause is the IDE serial monitor
echo         holding the port. Close it and run again.
goto fail

:fail
echo.
pause
exit /b 1

:done
echo.
pause
exit /b 0
