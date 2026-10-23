@echo off
rem Usage: build.bat [arm64|x64]   (default: native architecture)
setlocal
set TARGET=%1
set HOST=amd64
if /i "%PROCESSOR_ARCHITECTURE%"=="ARM64" set HOST=arm64
if "%TARGET%"=="" (if "%HOST%"=="arm64" (set TARGET=arm64) else (set TARGET=x64))
set VCTARGET=%TARGET%
if /i "%TARGET%"=="x64" set VCTARGET=amd64
if /i "%HOST%"=="%VCTARGET%" (set VCARG=%HOST%) else (set VCARG=%HOST%_%VCTARGET%)

for /f "usebackq tokens=*" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath`) do set VS=%%i
if not defined VS (echo Visual Studio with C++ tools not found & exit /b 1)
call "%VS%\VC\Auxiliary\Build\vcvarsall.bat" %VCARG% >nul || exit /b 1

set OUT=%~dp0build\%TARGET%
if not exist "%OUT%" mkdir "%OUT%"
cl /nologo /O2 /GL /std:c++20 /EHsc /W3 /utf-8 /MT /DUNICODE /D_UNICODE ^
   /Fo"%OUT%\\" /Fe"%OUT%\snip.exe" "%~dp0src\*.cpp" ^
   /link /LTCG /SUBSYSTEM:WINDOWS /OPT:REF /OPT:ICF ^
   user32.lib gdi32.lib d2d1.lib dwrite.lib windowscodecs.lib ole32.lib shell32.lib ^
   shcore.lib dwmapi.lib comdlg32.lib advapi32.lib taskschd.lib oleaut32.lib || exit /b 1
echo Built %OUT%\snip.exe
