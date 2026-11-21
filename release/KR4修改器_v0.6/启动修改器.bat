@echo off
chcp 65001 >nul
cd /d "%~dp0"
title KR4 Mod Injector

echo ==================================================
echo   KR4 Mod  v0.6  (dll build v24)
echo ==================================================
echo   1. Start the game first (Steam -^> Play)
echo   2. Run this file
echo   3. Press INSERT in game to toggle the panel
echo      Press END to unload all hooks
echo ==================================================
echo.

if not exist "%~dp0kr4mod.dll" (
    echo [x] kr4mod.dll not found next to this script.
    echo     Keep all files in the same folder.
    pause
    exit /b 1
)

kr4injector.exe --name "Kingdom Rush Vengeance.exe" --dll "%~dp0kr4mod.dll" --wait 120
set RC=%ERRORLEVEL%

echo.
if "%RC%"=="0" (
    echo [OK] Injected. Switch to the game and press INSERT.
) else (
    echo [x] Injection failed, exit code = %RC%
    echo     1=usage  2=process not found  3=open/alloc/write failed
    echo     4=remote thread failed  5=LoadLibrary returned NULL
    echo     See 使用说明.md section 9 for details.
)
echo.
pause
