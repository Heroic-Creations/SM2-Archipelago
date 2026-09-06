@echo off
title Spider-Man 2 - Archipelago client
cd /d "%~dp0client"
where py >nul 2>nul && (set PY=py -3) || (set PY=python)
%PY% -c "import websockets" 2>nul || (
  echo Installing the one Python package the client needs...
  %PY% -m pip install --user websockets
)
echo.
echo Client starting. Leave this window open. In the game, press F8 to connect.
echo.
%PY% -u client.py
pause
