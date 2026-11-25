@echo off
REM NDM - stage the browser extensions into dist\ (Chrome/Edge unpacked + .crx, Firefox .xpi)
setlocal
set "ROOT=%~dp0"
set "DIST=%ROOT%..\..\dist"
set "KEYFILE=%ROOT%..\..\keys\ndm_ext_private.pem"

echo [NDM] Packaging browser extensions...

REM -- Chrome / Edge (MV3) ----------------------------------------------------
set "CHROME_OUT=%DIST%\NDMExt_Chrome"
if exist "%CHROME_OUT%" rd /s /q "%CHROME_OUT%"
mkdir "%CHROME_OUT%"
xcopy /E /Y /Q "%ROOT%chrome\*" "%CHROME_OUT%\" >nul
echo [NDM] Chrome/Edge extension staged at %CHROME_OUT%  (ID dbphbdbafcniaifdaeikngfgmdmeeobf)

REM Optional .crx, signed with the same key so the ID stays fixed.
set "CHROME_EXE="
for %%P in ("%ProgramFiles%\Google\Chrome\Application\chrome.exe" "%ProgramFiles(x86)%\Google\Chrome\Application\chrome.exe" "%LocalAppData%\Google\Chrome\Application\chrome.exe") do if exist %%P set "CHROME_EXE=%%~P"
if defined CHROME_EXE if exist "%KEYFILE%" (
    "%CHROME_EXE%" --pack-extension="%CHROME_OUT%" --pack-extension-key="%KEYFILE%" >nul 2>&1
    if exist "%CHROME_OUT%.crx" move /y "%CHROME_OUT%.crx" "%DIST%\NDMGCExt.crx" >nul && echo [NDM] NDMGCExt.crx created.
)

REM -- Firefox (MV2) ----------------------------------------------------------
set "FF_OUT=%DIST%\NDMExt_Firefox"
if exist "%FF_OUT%" rd /s /q "%FF_OUT%"
mkdir "%FF_OUT%"
xcopy /E /Y /Q "%ROOT%firefox\*" "%FF_OUT%\" >nul
powershell -NoProfile -Command "Compress-Archive -Path '%FF_OUT%\*' -DestinationPath '%DIST%\NDMExt.zip' -Force; Move-Item -Force '%DIST%\NDMExt.zip' '%DIST%\NDMExt.xpi'"
echo [NDM] NDMExt.xpi created (unsigned: load via about:debugging, or sign on addons.mozilla.org).

echo.
echo [NDM] Extension packaging complete.
endlocal
