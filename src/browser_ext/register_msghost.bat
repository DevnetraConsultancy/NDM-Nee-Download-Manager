@echo off
REM NDM - developer registration of the native messaging host (current user only).
REM The installer does this for all users (HKLM, 32- and 64-bit views).
REM Usage: register_msghost.bat [folder containing NDMMsgHost.exe]   (default: build\bin)
REM        register_msghost.bat /u   (remove the HKCU registration)

setlocal
set "HOST_NAME=com.devnetra.ndm"
set "CHROME_EXT_ID=dbphbdbafcniaifdaeikngfgmdmeeobf"
set "FIREFOX_EXT_ID=ndm@devnetra.com"

if /i "%~1"=="/u" (
    for %%K in ("Google\Chrome" "Microsoft\Edge" "Chromium" "Mozilla") do reg delete "HKCU\Software\%%~K\NativeMessagingHosts\%HOST_NAME%" /f >nul 2>&1
    echo [NDM] Native messaging host unregistered for the current user.
    goto :eof
)

if "%~1"=="" (set "HOST_DIR=%~dp0..\..\build\bin") else (set "HOST_DIR=%~1")
for %%I in ("%HOST_DIR%") do set "HOST_DIR=%%~fI"
set "HOST_EXE=%HOST_DIR%\NDMMsgHost.exe"
if not exist "%HOST_EXE%" (
    echo [NDM] NDMMsgHost.exe not found in %HOST_DIR%
    exit /b 1
)
set "HOST_JSON=%HOST_EXE:\=\%"
set "CHROME_MANIFEST=%HOST_DIR%\NDMMsgHost_chrome.json"
set "FF_MANIFEST=%HOST_DIR%\NDMMsgHost_firefox.json"

(
echo {
echo   "name": "%HOST_NAME%",
echo   "description": "Nee Download Manager Native Messaging Host",
echo   "path": "%HOST_JSON%",
echo   "type": "stdio",
echo   "allowed_origins": [ "chrome-extension://%CHROME_EXT_ID%/" ]
echo }
) > "%CHROME_MANIFEST%"

(
echo {
echo   "name": "%HOST_NAME%",
echo   "description": "Nee Download Manager Native Messaging Host",
echo   "path": "%HOST_JSON%",
echo   "type": "stdio",
echo   "allowed_extensions": [ "%FIREFOX_EXT_ID%" ]
echo }
) > "%FF_MANIFEST%"

for %%K in ("Google\Chrome" "Microsoft\Edge" "Chromium") do reg add "HKCU\Software\%%~K\NativeMessagingHosts\%HOST_NAME%" /ve /t REG_SZ /d "%CHROME_MANIFEST%" /f >nul
reg add "HKCU\Software\Mozilla\NativeMessagingHosts\%HOST_NAME%" /ve /t REG_SZ /d "%FF_MANIFEST%" /f >nul

echo [NDM] Registered %HOST_EXE%
echo       Chrome/Edge extension ID: %CHROME_EXT_ID%  (fixed by "key" in chrome\manifest.json)
echo       Firefox add-on ID:        %FIREFOX_EXT_ID%
endlocal
