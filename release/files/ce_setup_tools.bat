@echo off
rem Double-click this: downloads ffmpeg and Chrome for the video / audio players and the web browser (asks first).
rem It only starts ce_setup_tools.ps1 - Windows doesn't run .ps1 files by double-click.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0ce_setup_tools.ps1"
pause
