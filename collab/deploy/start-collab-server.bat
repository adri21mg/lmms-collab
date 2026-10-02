@echo off
rem LMMS collaboration server for Windows: keeps running while this window is open.
rem Projects are stored in the "collab-server-data" folder next to this file.
rem Close this window (or press Ctrl+C) to stop it; projects are saved first.
rem
rem It asks for a password everybody will need to connect (press Enter for none).
rem The connection is not encrypted: prefer Tailscale over opening the port on your router. See deploy/README.md.

cd /d "%~dp0"
set "LMMS_COLLAB_PASSWORD="
set /p "LMMS_COLLAB_PASSWORD=Password for this server (Enter for none): "
cls
echo LMMS collaboration server - close this window to stop it.
echo.
lmms-collab-server.exe --listen 0.0.0.0 --port 42871 --data "%~dp0collab-server-data"
pause
