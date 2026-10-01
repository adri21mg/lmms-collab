@echo off
rem LMMS collaboration server for Windows: keeps running while this window is open.
rem Projects are stored in the "collab-server-data" folder next to this file.
rem Close this window (or press Ctrl+C) to stop it; projects are saved first.
rem
rem By default anyone who can reach this computer on port 42871 can join (passwords come later):
rem prefer Tailscale over opening the port on your router. See deploy/README.md.

cd /d "%~dp0"
lmms-collab-server.exe --listen 0.0.0.0 --port 42871 --data "%~dp0collab-server-data"
pause
