@echo off
rem notepad_demo.bat — the canonical first run of lua-injector.
rem
rem Starts notepad, attaches the Lua host to it, and dumps every module
rem the process has loaded, then leaves the host attached so you can
rem reconnect and play in the REPL.
rem
rem Run from the lua-injector folder (where the .exe files live):
rem     examples\notepad_demo.bat
setlocal

if not exist lua-injector-x64.exe (
    echo Run this from the lua-injector folder, after building.
    echo   make          ^(or build.bat on Windows with MSVC^)
    exit /b 1
)

echo Starting notepad...
start "" notepad.exe
timeout /t 1 /nobreak >nul

for /f %%p in ('powershell -NoProfile -Command "(Get-Process notepad -ErrorAction SilentlyContinue ^| Select-Object -First 1).Id"') do set PID=%%p
if "%PID%"=="" (
    echo Could not find notepad.exe
    exit /b 1
)

echo Attaching to notepad.exe pid %PID%
lua-injector-x64.exe --pid %PID% --script examples\process_info.lua --stay

echo.
echo The host is still inside notepad. Reconnect any time:
echo   lua-injector-x64.exe --pid %PID%
endlocal
