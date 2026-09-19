@echo off
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0Install.ps1" %*
if errorlevel 1 (
    echo.
    echo Audio Monitor installation failed. See the error above.
    exit /b 1
)
