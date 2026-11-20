@echo off
setlocal EnableExtensions EnableDelayedExpansion

title VRecorder V2.2 - Build Script
cd /d "%~dp0"

echo ==================================================
echo   VRecorder V2.2 - Native C++ Studio Builder
echo ==================================================
echo.

set "CMAKE_EXE="
where cmake >nul 2>&1
if %errorlevel%==0 (
    for /f "delims=" %%I in ('where cmake') do (
        set "CMAKE_EXE=%%I"
        goto :cmake_found
    )
)
if exist "%ProgramFiles%\CMake\bin\cmake.exe" (
    set "CMAKE_EXE=%ProgramFiles%\CMake\bin\cmake.exe"
) else (
    echo [ERROR] CMake tidak ditemukan. Install CMake 3.20+ terlebih dahulu.
    pause
    exit /b 1
)

:cmake_found
set "VS_PATH="
set "VS_VER="
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if exist "%VSWHERE%" (
    for /f "usebackq delims=" %%I in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VS_PATH=%%I"
    for /f "usebackq delims=" %%V in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationVersion`) do set "VS_VER=%%V"
)
if "%VS_PATH%"=="" (
    echo [ERROR] MSVC C++ Build Tools tidak ditemukan.
    pause
    exit /b 1
)

tasklist /FI "IMAGENAME eq VRecorder.exe" 2>nul | find /I "VRecorder.exe" >nul
if %errorlevel%==0 (
    echo [!] VRecorder.exe sedang berjalan dan akan ditutup sebelum rebuild...
    taskkill /F /IM VRecorder.exe >nul 2>&1
    timeout /t 1 /nobreak >nul
)

set "CMAKE_GEN=Visual Studio 17 2022"
echo %VS_VER% | findstr /B "16." >nul && set "CMAKE_GEN=Visual Studio 16 2019"
echo %VS_VER% | findstr /B "18." >nul && set "CMAKE_GEN=Visual Studio 18 2026"

echo [*] Configuring CMake ^(%CMAKE_GEN% - x64^)...
"%CMAKE_EXE%" -B build -G "%CMAKE_GEN%" -A x64
if errorlevel 1 (
    echo [ERROR] CMake configure gagal.
    pause
    exit /b 1
)

echo.
echo [*] Building Release x64...
"%CMAKE_EXE%" --build build --config Release
if errorlevel 1 (
    echo [ERROR] Build gagal.
    pause
    exit /b 1
)

echo.
echo ==================================================
echo   Build selesai: build\Release\VRecorder.exe
echo ==================================================
pause
exit /b 0
