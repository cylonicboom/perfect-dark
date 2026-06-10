@echo off
rem ===========================================================================
rem  soak_oneclick.bat - one-click prop-sync soak (docs/PORT_NET_SOAK.md).
rem
rem  Launches the dedicated SERVER in its own window, then runs the headless
rem  CLIENT in THIS window, both for a timed run; when the window elapses it
rem  prints the combined server<->client manifest-parity verdict.
rem
rem  Usage:   soak_oneclick.bat [minutes]        (default 30)
rem  Binary:  build-server\pd-server.x86_64.exe  (hosts AND joins; the ROM
rem           lives in build-server\data). Rebuild it after code changes:
rem           MSYS2 MinGW x64 shell -> cmake --build build-server -j
rem ===========================================================================
setlocal

set MINUTES=%~1
if "%MINUTES%"=="" set MINUTES=30

set BASH=C:\msys64\usr\bin\bash.exe
for %%I in ("%~dp0..\..") do set REPO=%%~fI
set BIN=build-server/pd-server.x86_64.exe

if not exist %BASH% echo error: MSYS2 not found at C:\msys64 && pause && exit /b 1
if not exist "%REPO%\build-server\pd-server.x86_64.exe" echo error: build-server\pd-server.x86_64.exe missing - build it first && pause && exit /b 1

rem MSYS2 MinGW x64 environment for every bash below (inherited by the
rem server window too). CHERE_INVOKING keeps bash -l in the repo root.
set MSYSTEM=MINGW64
set CHERE_INVOKING=1
cd /d "%REPO%"

echo === prop-sync soak: %MINUTES% min, server window + this window as client ===
start "PD SOAK SERVER" /D "%REPO%" cmd /k %BASH% -lc 'bash tools/soak/run_server.sh %BIN% 27100 %MINUTES%'

echo waiting 12s for the server to boot before connecting the client...
timeout /t 12 /nobreak >nul

%BASH% -lc 'bash tools/soak/run_client.sh 127.0.0.1:27100 %BIN% %MINUTES%'

echo.
echo === combined server-client parity verdict (newest CSV pair) ===
%BASH% -lc 'bash tools/soak/parity_latest.sh'

echo.
echo (server window stays open with its own verdict - close it when done)
pause
