@echo off
setlocal EnableExtensions EnableDelayedExpansion
cd /d "%~dp0"
title Junkbot asset generator

set "TOOLS=%LOCALAPPDATA%\JunkbotTools"
set "BIN=%TOOLS%\bin"
set "LOCALPY=%TOOLS%\python"
set "LOCALFFMPEG=%TOOLS%\ffmpeg"
set "LOCALGIT=%TOOLS%\git"
set "PATH=%BIN%;%PATH%"
mkdir "%BIN%" >nul 2>&1

echo.
echo Junkbot asset generator
echo =======================
echo.
echo On the first run this downloads free tools into:
echo   %TOOLS%
echo.

echo [1/4] Checking Python...
call :find_python
if errorlevel 1 call :install_python
if errorlevel 1 goto :failed
call :py -m pip --version >nul 2>&1
if errorlevel 1 (
    echo Installing pip...
    call :py -m ensurepip --upgrade >nul 2>&1
)
call :py -m pip install --disable-pip-version-check --user --upgrade pillow numpy
if errorlevel 1 goto :failed

echo [2/4] Checking ffmpeg...
call :find_ffmpeg
if errorlevel 1 goto :failed

echo [3/4] Checking Git...
call :find_git
if errorlevel 1 goto :failed

echo [4/4] Building junkbot.otr...
set "FIRST=%~1"
if not defined FIRST (
    call :py "%~dp0make_assets.py"
) else (
    if "%FIRST:~0,1%"=="-" (
        call :py "%~dp0make_assets.py" %*
    ) else (
        set "SRC=%~1"
        if /I "%~x1"==".dir" (
            call :py "%~dp0make_assets.py" --dir "!SRC!"
        ) else (
            call :py "%~dp0make_assets.py" --code "!SRC!"
        )
    )
)
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

:find_ffmpeg
where ffmpeg >nul 2>&1
if not errorlevel 1 exit /b 0
if exist "%LOCALFFMPEG%\bin\ffmpeg.exe" (
    set "PATH=%LOCALFFMPEG%\bin;%PATH%"
    exit /b 0
)
set "FFMPEGZIP=%TOOLS%\ffmpeg.zip"
echo Downloading ffmpeg...
call :download "https://www.gyan.dev/ffmpeg/builds/ffmpeg-release-essentials.zip" "%FFMPEGZIP%"
if errorlevel 1 exit /b 1
echo Unpacking ffmpeg...
powershell -NoProfile -ExecutionPolicy Bypass -Command "param([string]$z,[string]$d) Expand-Archive -Path $z -DestinationPath $d -Force" "%FFMPEGZIP%" "%LOCALFFMPEG%"
if errorlevel 1 exit /b 1
set "FFMPEGBIN="
for /r "%LOCALFFMPEG%" %%F in (ffmpeg.exe) do set "FFMPEGBIN=%%~dpF"
if not defined FFMPEGBIN exit /b 1
set "PATH=%FFMPEGBIN%;%PATH%"
exit /b 0

:find_git
where git >nul 2>&1
if not errorlevel 1 exit /b 0
if exist "%LOCALGIT%\cmd\git.exe" (
    set "PATH=%LOCALGIT%\cmd;%PATH%"
    exit /b 0
)
set "GITVER=2.47.1"
set "GITFILE=Git-%GITVER%-64-bit.exe"
if /I "%PROCESSOR_ARCHITECTURE%"=="x86" set "GITFILE=Git-%GITVER%-32-bit.exe"
set "GITINSTALLER=%TOOLS%\%GITFILE%"
echo Downloading Git...
call :download "https://github.com/git-for-windows/git/releases/download/v%GITVER%.windows.1/%GITFILE%" "%GITINSTALLER%"
if errorlevel 1 exit /b 1
echo Installing Git into %LOCALGIT%...
start "Installing Git" /wait "%GITINSTALLER%" /VERYSILENT /NORESTART /NOCANCEL /SP- /CLOSEAPPLICATIONS /DIR="%LOCALGIT%"
if errorlevel 1 exit /b 1
if not exist "%LOCALGIT%\cmd\git.exe" exit /b 1
set "PATH=%LOCALGIT%\cmd;%PATH%"
exit /b 0

:download
powershell -NoProfile -ExecutionPolicy Bypass -Command "param([string]$u,[string]$o) [Net.ServicePointManager]::SecurityProtocol=[Net.SecurityProtocolType]::Tls12; Invoke-WebRequest -Uri $u -OutFile $o -UseBasicParsing" "%~1" "%~2"
exit /b %ERRORLEVEL%
