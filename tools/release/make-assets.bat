@echo off
setlocal EnableExtensions EnableDelayedExpansion
cd /d "%~dp0"
title Junkbot asset generator

set "TOOLS=%LOCALAPPDATA%\JunkbotTools"
set "LOCALPY=%TOOLS%\python"

echo.
echo Junkbot asset generator
echo =======================
echo.

echo Checking Python...
call :find_python
if errorlevel 1 call :install_python
if errorlevel 1 goto :failed

call :py "%~dp0make_assets.py" %*
set "RC=%ERRORLEVEL%"
if not "%RC%"=="0" goto :failed

echo.
echo Done. Copy junkbot.nro and junkbot.otr to SD:/switch/junkbot/ on your SD card.
echo.
exit /b 0

:failed
echo.
echo Asset generation failed. See the messages above for details.
echo.
exit /b 1

:py
"%PYEXE%" %PYARGS% %*
exit /b %ERRORLEVEL%

:find_python
where py >nul 2>&1
if not errorlevel 1 (
    set "PYEXE=py"
    set "PYARGS=-3"
    call :py -c "import sys; raise SystemExit(0 if sys.version_info >= (3, 10) else 1)"
    if not errorlevel 1 exit /b 0
)
where python >nul 2>&1
if not errorlevel 1 (
    set "PYEXE=python"
    set "PYARGS="
    call :py -c "import sys; raise SystemExit(0 if sys.version_info >= (3, 10) else 1)"
    if not errorlevel 1 exit /b 0
)
if exist "%LOCALPY%\python.exe" (
    set "PYEXE=%LOCALPY%\python.exe"
    set "PYARGS="
    call :py -c "import sys; raise SystemExit(0 if sys.version_info >= (3, 10) else 1)"
    if not errorlevel 1 exit /b 0
)
exit /b 1

:install_python
set "PYVER=3.12.10"
set "PYFILE=python-%PYVER%.exe"
if /I "%PROCESSOR_ARCHITECTURE%"=="AMD64" set "PYFILE=python-%PYVER%-amd64.exe"
if /I "%PROCESSOR_ARCHITECTURE%"=="ARM64" set "PYFILE=python-%PYVER%-arm64.exe"
set "PYINSTALLER=%TOOLS%\%PYFILE%"
echo Downloading Python %PYVER%...
call :download "https://www.python.org/ftp/python/%PYVER%/%PYFILE%" "%PYINSTALLER%"
if errorlevel 1 exit /b 1
echo Installing Python %PYVER% into %LOCALPY%...
start "Installing Python" /wait "%PYINSTALLER%" /quiet InstallAllUsers=0 PrependPath=0 Include_pip=1 Include_test=0 TargetDir="%LOCALPY%"
if errorlevel 1 exit /b 1
set "PYEXE=%LOCALPY%\python.exe"
set "PYARGS="
call :py -c "import sys" >nul 2>&1
exit /b %ERRORLEVEL%

:download
if exist "%~2" del /q "%~2"
set "DL_URL=%~1"
set "DL_OUT=%~2"
powershell -NoProfile -ExecutionPolicy Bypass -Command "$ProgressPreference='SilentlyContinue'; [Net.ServicePointManager]::SecurityProtocol=[Net.SecurityProtocolType]::Tls12; Invoke-WebRequest -Uri $env:DL_URL -OutFile $env:DL_OUT -UseBasicParsing -ErrorAction Stop"
exit /b %ERRORLEVEL%
