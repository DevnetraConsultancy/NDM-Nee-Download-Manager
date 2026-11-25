@echo off
setlocal enabledelayedexpansion

set "ROOT=%~dp0"
set "BUILD=%ROOT%build"
set "DIST=%ROOT%dist"

echo [NDM Build] Configuring (x64)...
cmake -S "%ROOT%" -B "%BUILD%" -G "Visual Studio 17 2022" -A x64 ^
      -DCMAKE_BUILD_TYPE=Release
if %errorlevel% neq 0 goto :err

echo [NDM Build] Building...
cmake --build "%BUILD%" --config Release --parallel
if %errorlevel% neq 0 goto :err

echo [NDM Build] Staging dist...
if not exist "%DIST%" mkdir "%DIST%"

xcopy /Y "%BUILD%\bin\Release\NDMan.exe"                "%DIST%\" 2>nul
xcopy /Y "%BUILD%\bin\Release\ndmsetup643build1.exe"    "%DIST%\" 2>nul
xcopy /Y "%BUILD%\bin\Release\Uninstall.exe"            "%DIST%\" 2>nul
xcopy /Y "%BUILD%\bin\Release\NDMShellExt.dll"          "%DIST%\" 2>nul
xcopy /Y "%BUILD%\bin\Release\NDMMsgHost.exe"           "%DIST%\" 2>nul

echo.
echo [NDM Build] Done. Output in %DIST%
goto :end

:err
echo.
echo [NDM Build] FAILED (exit code %errorlevel%).
exit /b 1

:end
endlocal
