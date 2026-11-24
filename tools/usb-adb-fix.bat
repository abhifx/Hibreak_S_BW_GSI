@echo off
rem usb-adb-fix.bat -- one elevated launcher for both USB-adb fixes, in order:
rem   1. release the device from usbipd (so Windows can bind it at all)
rem   2. write the Android ADB interface GUID (so adb can see the WinUSB device)
rem
rem Double-click, or run from a normal cmd window. Accept the UAC prompt.
setlocal
set "HERE=%~dp0"

net session >nul 2>&1
if not "%errorlevel%"=="0" (
    echo Requesting administrator rights...
    powershell -NoProfile -ExecutionPolicy Bypass -Command "Start-Process -FilePath '%~f0' -Verb RunAs"
    exit /b
)

echo ============================================================
echo Step 1: release from usbipd
echo ============================================================
powershell -NoProfile -ExecutionPolicy Bypass -File "%HERE%fix-usb-adb.ps1" -NoAdbRestart
echo.
echo ============================================================
echo Step 2: Android ADB interface GUID
echo ============================================================
powershell -NoProfile -ExecutionPolicy Bypass -File "%HERE%fix-usb-adb-win.ps1"
set "RC=%errorlevel%"

echo.
echo ============================================================
echo Done. Final adb state:
adb devices -l
echo.
pause
exit /b %RC%
