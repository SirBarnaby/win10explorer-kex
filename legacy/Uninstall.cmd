@echo off
"%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe" -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%~dp0Uninstall.ps1"
if errorlevel 1 (echo Uninstall did not finish. See the message above.)
pause
