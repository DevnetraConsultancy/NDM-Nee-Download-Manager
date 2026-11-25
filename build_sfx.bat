@echo off
REM =========================================================
REM NDM Full SFX Build (x64)
REM Produces: dist\NDM-1.0.5-x64-Setup.exe  (single standalone EXE)
REM
REM Usage: build_sfx.bat [output.exe]
REM   Bundles third_party\*.exe (+ licence texts) into <install>\tools\,
REM   plus LICENSE.txt and a generated THIRD_PARTY_NOTICES.txt.
REM   A missing/empty third_party\ only prints a warning.
REM   Payload format: NDM-SFX v3 (LZMS / XPRESS_HUFF via cabinet.dll, SHA-256
REM   verified), see src\installer\ndm_sfx.h. Packing ~400 MB of tools takes
REM   a few minutes.
REM
REM Optional environment overrides:
REM   NDM_VCVARS          full path of vcvars64.bat   (default: found via vswhere)
REM   NDM_CMAKE           full path of cmake.exe      (default: PATH, then %ProgramFiles%\CMake)
REM   NDM_NINJA           full path of ninja.exe      (default: PATH, then WinGet package)
REM   NDM_BUILD_DIR       build directory             (default: <repo>\build, wiped first)
REM   NDM_SIGN_CERT       .pfx to Authenticode-sign the final EXE with (unset: unsigned)
REM   NDM_SIGN_PASSWORD   password of NDM_SIGN_CERT (optional)
REM   NDM_SIGN_TIMESTAMP  RFC 3161 timestamp URL      (default: http://timestamp.digicert.com)
REM The EXE is signed AFTER the payload is appended; the stub finds its
REM payload in front of the certificate table.
REM =========================================================
setlocal EnableExtensions

set "ROOT=%~dp0"
set "OUTEXE=%~1"
if "%OUTEXE%"=="" set "OUTEXE=%ROOT%dist\NDM-1.0.5-x64-Setup.exe"
set "BUILD=%NDM_BUILD_DIR%"
if "%BUILD%"=="" set "BUILD=%ROOT%build"

REM --- Toolchain: x64 MSVC environment ----------------------
set "VCVARS=%NDM_VCVARS%"
if not "%VCVARS%"=="" goto :have_vcvars
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" goto :no_vcvars
set "VSINST="
for /f "usebackq delims=" %%I in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSINST=%%I"
if "%VSINST%"=="" goto :no_vcvars
set "VCVARS=%VSINST%\VC\Auxiliary\Build\vcvars64.bat"
:have_vcvars
if not exist "%VCVARS%" goto :no_vcvars
call "%VCVARS%" >nul
if errorlevel 1 ( echo vcvars64 failed: "%VCVARS%" & exit /b 1 )
if /i not "%VSCMD_ARG_TGT_ARCH%"=="x64" ( echo Not an x64 toolchain: "%VCVARS%" & exit /b 1 )

REM --- CMake ------------------------------------------------
set "CMAKE=%NDM_CMAKE%"
if not "%CMAKE%"=="" goto :have_cmake
for /f "delims=" %%I in ('where cmake.exe 2^>nul') do if not defined CMAKE set "CMAKE=%%I"
if "%CMAKE%"=="" set "CMAKE=%ProgramFiles%\CMake\bin\cmake.exe"
:have_cmake
if not exist "%CMAKE%" ( echo CMake not found - install it or set NDM_CMAKE & exit /b 1 )

REM --- Ninja ------------------------------------------------
set "NINJA=%NDM_NINJA%"
if not "%NINJA%"=="" goto :have_ninja
for /f "delims=" %%I in ('where ninja.exe 2^>nul') do if not defined NINJA set "NINJA=%%I"
if not "%NINJA%"=="" goto :have_ninja
for /d %%D in ("%LOCALAPPDATA%\Microsoft\WinGet\Packages\Ninja-build.Ninja_*") do if exist "%%D\ninja.exe" set "NINJA=%%D\ninja.exe"
:have_ninja
if not exist "%NINJA%" ( echo Ninja not found - install it or set NDM_NINJA & exit /b 1 )

echo Using: "%VCVARS%"
echo        "%CMAKE%"
echo        "%NINJA%"

REM --- Step 1: Clean + configure ---------------------------
if exist "%BUILD%" rmdir /s /q "%BUILD%"
mkdir "%BUILD%"
REM NOTE: %ROOT% ends with "\"; quoting it bare ("%ROOT%") escapes the closing quote.
"%CMAKE%" -S "%ROOT%." -B "%BUILD%" -G Ninja ^
    -DCMAKE_BUILD_TYPE=Release ^
    -DCMAKE_CXX_COMPILER=cl ^
    -DCMAKE_MAKE_PROGRAM="%NINJA%"
if errorlevel 1 ( echo CMake configure failed & exit /b 1 )

REM --- Step 2: Build all targets ---------------------------
"%CMAKE%" --build "%BUILD%" -- -j4
if errorlevel 1 ( echo Build failed & exit /b 1 )

REM --- Step 3: Append SFX payload to installer stub --------
powershell -NoProfile -ExecutionPolicy Bypass -File "%ROOT%tools\make_sfx_payload.ps1" ^
    -StubExe       "%BUILD%\bin\ndmsetup643build1.exe" ^
    -OutputExe     "%OUTEXE%" ^
    -BinDir        "%BUILD%\bin" ^
    -ExtDir        "%ROOT%src\browser_ext" ^
    -ThirdPartyDir "%ROOT%third_party"
if errorlevel 1 ( echo SFX packing failed & exit /b 1 )

REM --- Step 4: Optional Authenticode signature -------------
if "%NDM_SIGN_CERT%"=="" goto :unsigned
if not exist "%NDM_SIGN_CERT%" ( echo NDM_SIGN_CERT not found: "%NDM_SIGN_CERT%" & exit /b 1 )
set "TSURL=%NDM_SIGN_TIMESTAMP%"
if "%TSURL%"=="" set "TSURL=http://timestamp.digicert.com"
set "SIGNPW="
if not "%NDM_SIGN_PASSWORD%"=="" set SIGNPW=/p "%NDM_SIGN_PASSWORD%"
signtool sign /fd sha256 /f "%NDM_SIGN_CERT%" %SIGNPW% /tr "%TSURL%" /td sha256 /d "Nee Download Manager Setup" "%OUTEXE%"
if errorlevel 1 ( echo Signing failed & exit /b 1 )
signtool verify /pa "%OUTEXE%" >nul 2>&1
if errorlevel 1 ( echo WARNING: signature does not verify against a trusted root ^(test certificate?^) ) else ( echo Signature verified. )
set "SIGNSTATE=signed"
goto :report
:unsigned
echo NDM_SIGN_CERT not set: installer is unsigned.
set "SIGNSTATE=unsigned"

REM --- Step 5: Report --------------------------------------
:report
echo.
echo ========================================================
echo  NDM Build Complete (%SIGNSTATE%)
echo  Standalone installer: %OUTEXE%
echo ========================================================
for %%F in ("%OUTEXE%") do echo Size: %%~zF bytes
certutil -hashfile "%OUTEXE%" SHA256 | findstr /v /c:"CertUtil"
endlocal
exit /b 0

:no_vcvars
echo Visual Studio 2022 C++ x64 build tools not found - install them or set NDM_VCVARS
exit /b 1
