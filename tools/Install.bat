@echo off
rem Double-click installer for the SCCT Unofficial Remaster.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0install.ps1" %*
pause
