@echo off
rem Installs or updates Party Board on a Meta Quest plugged into this PC (install-quest.ps1).
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0install-quest.ps1" %*
pause
