@echo off
setlocal

net session >nul 2>&1
if not "%~1"=="__elevated" if errorlevel 1 (
	powershell.exe -NoProfile -ExecutionPolicy Bypass -Command "Start-Process -FilePath '%~f0' -ArgumentList '__elevated' -Verb RunAs"
	exit /b %ERRORLEVEL%
)

if "%~1"=="__elevated" shift

powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0restart-driver.ps1" %*
set EXITCODE=%ERRORLEVEL%
endlocal & exit /b %EXITCODE%