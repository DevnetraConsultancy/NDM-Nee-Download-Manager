@echo off
for /f "usebackq delims=" %%I in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath`) do set "VSDIR=%%I"
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul 2>nul
cd /d "%~dp0"
set S=%~dp0..\..\src
if not defined NDM_BUILD set "NDM_BUILD=%~dp0..\..\build_dev"
set L=%NDM_BUILD%\lib
cl /nologo /W4 /EHsc /std:c++17 /utf-8 /O2 /MT /DUNICODE /D_UNICODE /DNOMINMAX /I%S%\common /I%S%\core /I%S%\network /I%S%\media /I%S%\msghost /I%S%\torrent mediatest.cpp /Fe:mediatest.exe /link %L%\ndm_msghost_lib.lib %L%\ndm_core.lib %L%\ndm_network.lib %L%\ndm_common.lib wininet.lib winhttp.lib shell32.lib advapi32.lib user32.lib ole32.lib shlwapi.lib crypt32.lib bcrypt.lib ws2_32.lib urlmon.lib
