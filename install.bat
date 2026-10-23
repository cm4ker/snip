@echo off
rem Builds Snip, installs it to %LOCALAPPDATA%\Programs\Snip and registers an elevated logon task
rem (Task Scheduler, "run with highest privileges"), then starts it. Shows one UAC prompt.
setlocal
call "%~dp0build.bat" %1 || exit /b 1
set TARGET=%1
if "%TARGET%"=="" (if /i "%PROCESSOR_ARCHITECTURE%"=="ARM64" (set TARGET=arm64) else (set TARGET=x64))
set DEST=%LOCALAPPDATA%\Programs\Snip
if not exist "%DEST%" mkdir "%DEST%"
if exist "%DEST%\snip.exe" "%DEST%\snip.exe" --exit
ping -n 3 127.0.0.1 >nul
copy /y "%~dp0build\%TARGET%\snip.exe" "%DEST%\snip.exe" >nul || exit /b 1
"%DEST%\snip.exe" --install-task
if errorlevel 1 (
    echo Elevation declined: falling back to a normal autostart without admin rights.
    reg add "HKCU\Software\Microsoft\Windows\CurrentVersion\Run" /v Snip /t REG_SZ /d "\"%DEST%\snip.exe\" --background" /f >nul
    start "" "%DEST%\snip.exe" --background
    exit /b 0
)
echo Installed to %DEST%\snip.exe, autostart: Task Scheduler task "Snip" (highest privileges).
