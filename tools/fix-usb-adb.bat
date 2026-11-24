@echo off
rem fix-usb-adb.bat -- release the HiBreak from usbipd so Windows can use it over USB.
rem Self-elevates via UAC, then runs fix-usb-adb.ps1 next to this file.
rem Pass -Share to re-bind for WSL:  fix-usb-adb.bat -Share
setlocal
set "PS1=%~dp0fix-usb-adb.ps1"
if not exist "%PS1%" (
    echo Missing "%PS1%"
    pause
    exit /b 1
)

net session >nul 2>&1
if not "%errorlevel%"=="0" (
    echo Requesting administrator rights...
    if "%~1"=="" (
        powershell -NoProfile -ExecutionPolicy Bypass -Command "Start-Process -FilePath '%~f0' -Verb RunAs"
    ) else (
        powershell -NoProfile -ExecutionPolicy Bypass -Command "Start-Process -FilePath '%~f0' -ArgumentList '%*' -Verb RunAs"
    )
    exit /b
)

powershell -NoProfile -ExecutionPolicy Bypass -File "%PS1%" %*
echo.
echo Exit code: %errorlevel%
pause
