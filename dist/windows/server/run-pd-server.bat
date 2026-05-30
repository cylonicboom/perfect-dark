@echo off
REM Perfect Dark headless dedicated server (Windows).
REM Runs in this folder; put pd.ntsc-final.z64 in a "data" subfolder, or point
REM --rom-file / --basedir at it. Edit the flags below to taste.
setlocal
cd /d "%~dp0"

pd-server.x86_64.exe ^
  --dedicated ^
  --port 27100 ^
  --maxclients 8 ^
  --server-name "Perfect Dark Dedicated" ^
  --playlist server_playlist.ini
REM   --admin-password yourpassword     (enables /admin remote control)

REM Keep the window open if the server exits so you can read any error.
pause
