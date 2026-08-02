@echo off
rem build.bat — build lua-injector with MSVC.
rem
rem Run from a "Developer Command Prompt" (x64 Native Tools) inside the
rem lua-injector folder.  Pass "32" as the argument to build the 32-bit
rem host DLL and injector instead (use the x86 Native Tools prompt then):
rem
rem     build.bat        -> bin\lua-host-x64.dll, bin\lua-injector-x64.exe
rem     build.bat 32     -> bin\lua-host-x86.dll, bin\lua-injector-x86.exe
setlocal

set BITS=64
if /I "%1"=="32" set BITS=32

set LUA=third_party\lua
set OUTDIR=build\msvc%BITS%
set BIN=bin

if not exist %OUTDIR% mkdir %OUTDIR%
if not exist %BIN% mkdir %BIN%

echo Compiling Lua 5.4.7 (%BITS%-bit) ...
for %%f in (%LUA%\*.c) do (
    if /I not "%%~nxf"=="lua.c" if /I not "%%~nxf"=="luac.c" (
        cl /nologo /c /O2 /MD /I%LUA% /Fo:%OUTDIR%\ %%f || exit /b 1
    )
)

echo Building lua-host-x%BITS%.dll ...
cl /nologo /O2 /MD /I%LUA% /Ihost /LD host\win_host.c host\luacore.c ^
    %OUTDIR%\*.obj /Fe:%BIN%\lua-host-x%BITS%.dll || exit /b 1

echo Building lua-injector-x%BITS%.exe ...
cl /nologo /O2 /MD /Iclient injector\injector.c client\common.c ^
    /Fe:%BIN%\lua-injector-x%BITS%.exe || exit /b 1

echo Building lua-injector-gui-x%BITS%.exe ...
cl /nologo /O2 /MD /Iclient gui\gui.c client\common.c ^
    /Fe:%BIN%\lua-injector-gui-x%BITS%.exe /link user32.lib gdi32.lib ^
    comctl32.lib comdlg32.lib shell32.lib /SUBSYSTEM:WINDOWS || exit /b 1

echo Building lua-test-game-x%BITS%.exe ...
cl /nologo /O2 /MD /Itest_game test_game\win_game.c test_game\game_logic.c ^
    /Fe:%BIN%\lua-test-game-x%BITS%.exe /link user32.lib gdi32.lib ^
    /SUBSYSTEM:WINDOWS || exit /b 1

echo.
echo Done. Binaries are in %BIN%\ :
echo   lua-injector-x%BITS%.exe
echo   lua-injector-gui-x%BITS%.exe
echo   lua-host-x%BITS%.dll
echo   lua-test-game-x%BITS%.exe
endlocal
