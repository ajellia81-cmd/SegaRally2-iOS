@echo off
rem Updates Sega Rally 2 Native to the latest version on GitHub and rebuilds it
rem (tools\update.ps1). No git needed. It keeps the ROM, save\, your own textures
rem and every other file that is not part of the repository.
rem Usage: update.bat [-Check] [-Force] [-NoBuild] [-FromGame]
rem -FromGame: the game's UPDATE NOW. Once updated it starts the game again
setlocal EnableDelayedExpansion
rem Started with a double click (cmd /c): keep the window open at the end
set "PAUSE_AT_END="
for %%a in (%cmdcmdline%) do if /i "%%~a"=="/c" set PAUSE_AT_END=1
set "FROM_GAME="
for %%a in (%*) do if /i "%%~a"=="-FromGame" set FROM_GAME=1
rem The rest is one block, read before it runs: the update may replace this file
(
    powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\update.ps1" %*
    set "RC=!errorlevel!"
    rem From the game and all went well: back into it, no key to press
    if defined FROM_GAME if "!RC!"=="0" if exist "%~dp0SegaRally2.exe" (
        start "" /D "%~dp0." "%~dp0SegaRally2.exe"
        exit /b 0
    )
    if defined PAUSE_AT_END pause
    exit /b !RC!
)
