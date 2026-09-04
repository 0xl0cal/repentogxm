@echo off
setlocal

where pyw.exe >nul 2>nul
if not errorlevel 1 (
  start "Isaac Vita Sync" pyw.exe -3 "%~dp0isaac_vita_sync_gui.py"
  exit /b 0
)

py.exe -3 "%~dp0isaac_vita_sync_gui.py"
if errorlevel 1 pause
