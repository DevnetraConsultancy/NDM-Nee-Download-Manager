@echo off
for /f "usebackq delims=" %%I in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath`) do set "VSDIR=%%I"
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul 2>nul
cd /d "%~dp0"
set S=%~dp0..\..\src
if not defined NDM_BUILD set "NDM_BUILD=%~dp0..\..\build_dev"
set L=%NDM_BUILD%\lib
cl /nologo /W4 /EHsc /std:c++17 /utf-8 /O2 /MT /DUNICODE /D_UNICODE /DNOMINMAX /I%S%\common /I%S%\network /I%S%\core etest.cpp /Fe:etest.exe /link %L%\ndm_network.lib %L%\ndm_common.lib winhttp.lib shell32.lib advapi32.lib user32.lib ole32.lib shlwapi.lib crypt32.lib bcrypt.lib urlmon.lib
cl /nologo /W4 /EHsc /std:c++17 /utf-8 /O2 /MT /DUNICODE /D_UNICODE /DNOMINMAX /I%S%\common /I%S%\network /I%S%\core /I%S%\media /I%S%\torrent /I%S%\scheduler qtest.cpp /Fe:qtest.exe /link %L%\ndm_core.lib %L%\ndm_network.lib %L%\ndm_common.lib winhttp.lib shell32.lib advapi32.lib user32.lib ole32.lib shlwapi.lib crypt32.lib bcrypt.lib urlmon.lib ws2_32.lib
cl /nologo /W4 /EHsc /std:c++17 /utf-8 /O2 /MT /DUNICODE /D_UNICODE /DNOMINMAX /I%S%\common /I%S%\ui /I%S%\core uitest.cpp %S%\ui\ndm_uilogic.cpp %S%\ui\ndm_theme.cpp %S%\core\ndm_config.cpp /Fe:uitest.exe /link %L%\ndm_common.lib user32.lib gdi32.lib shell32.lib ole32.lib oleaut32.lib oleacc.lib uuid.lib advapi32.lib comctl32.lib gdiplus.lib uxtheme.lib dwmapi.lib msimg32.lib crypt32.lib
cl /nologo /W4 /EHsc /std:c++17 /utf-8 /O2 /MT /DUNICODE /D_UNICODE /DNOMINMAX uninsttest.cpp /Fe:uninsttest.exe /link shell32.lib advapi32.lib user32.lib ole32.lib bcrypt.lib wtsapi32.lib
cl /nologo /W4 /EHsc /std:c++17 /utf-8 /O2 /MT /DUNICODE /D_UNICODE /DNOMINMAX /I%S%\common /I%S%\network /I%S%\core /I%S%\media plsrvtest.cpp /Fe:plsrvtest.exe /link %L%\ndm_network.lib %L%\ndm_common.lib winhttp.lib shell32.lib advapi32.lib user32.lib ole32.lib shlwapi.lib crypt32.lib bcrypt.lib ws2_32.lib urlmon.lib
