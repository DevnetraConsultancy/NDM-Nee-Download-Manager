@echo off
REM Developer build (no installer): dev_build.bat <build-dir> [ninja targets...]
REM Configures once (Release, Ninja, MSVC x64), then builds the given targets.
setlocal
set "ROOT=%~dp0.."
set "BUILD=%~1"
if "%BUILD%"=="" ( echo usage: dev_build.bat ^<build-dir^> [targets] & exit /b 2 )
shift
for /f "usebackq delims=" %%I in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath`) do set "VSDIR=%%I"
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul 2>nul
if not exist "%BUILD%\build.ninja" (
    cmake -S "%ROOT%" -B "%BUILD%" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=cl >nul
    if errorlevel 1 ( echo CMake configure failed & exit /b 1 )
)
set "TARGETS="
:collect
if "%~1"=="" goto :run
set "TARGETS=%TARGETS% %~1"
shift
goto :collect
:run
ninja -C "%BUILD%" %TARGETS%
