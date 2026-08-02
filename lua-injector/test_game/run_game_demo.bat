@echo off
rem run_game_demo.bat — the zero-thinking way to try the test game.
rem
rem Builds nothing; expects bin\ to already contain the binaries (see the
rem Makefile / build.bat).  Starts the game, waits for it to come up,
rem attaches the Lua host, and runs the demo script.  Then you can run
rem the other scripts against the same pid, or open a REPL:
rem
rem     lua-injector-x64.exe --pid <pid>
rem     lua-injector-x64.exe --pid <pid> --script test_game\scripts\game_autopilot.lua --stay
setlocal
cd /d %~dp0..

if not exist bin\lua-test-game-x64.exe (
    echo bin\lua-test-game-x64.exe not found. Build first:
    echo   make          ^(or build.bat on Windows with MSVC^)
    exit /b 1
)
if not exist bin\lua-injector-x64.exe (
    echo bin\lua-injector-x64.exe not found. Build first.
    exit /b 1
)

echo Starting the test game...
start "" bin\lua-test-game-x64.exe
timeout /t 2 /nobreak >nul

for /f %%p in ('powershell -NoProfile -Command "(Get-Process lua-test-game -ErrorAction SilentlyContinue ^| Select-Object -First 1).Id"') do set PID=%%p
if "%PID%"=="" (
    echo Could not find the game process.
    exit /b 1
)

echo Attaching to the game (pid %PID%) and running the demo script...
lua-injector-x64.exe --pid %PID% --script test_game\scripts\game_demo.lua --stay

echo.
echo The game window should now show a cyan Lua message.
echo More scripts to try:
echo   lua-injector-x64.exe --pid %PID% --script test_game\scripts\game_set_score.lua --stay
echo   lua-injector-x64.exe --pid %PID% --script test_game\scripts\game_freeze_lives.lua --stay
echo   lua-injector-x64.exe --pid %PID% --script test_game\scripts\game_autopilot.lua --stay
echo   lua-injector-x64.exe --pid %PID%
endlocal
