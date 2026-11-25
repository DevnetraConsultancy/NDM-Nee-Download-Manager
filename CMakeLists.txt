cmake_minimum_required(VERSION 3.20)
project(NDM VERSION 1.0.5 LANGUAGES CXX)
if(MSVC)
    enable_language(RC)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ---------------------------------------------------------------------------
# Target: Windows 10/11 x64 only (configure from a vcvars64 / x64 prompt or
# with -A x64).
# ---------------------------------------------------------------------------
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "NDM is built for x64 only. Use vcvars64.bat (Ninja) or -A x64 (Visual Studio generator).")
endif()

# ---------------------------------------------------------------------------
# Hardening (all targets)
#  - static CRT: no vcruntime140/msvcp140 next to the exe -> nothing to plant
#  - /DEPENDENTLOADFLAG:0x800 (LOAD_LIBRARY_SEARCH_SYSTEM32): the loader
#    resolves static imports from System32 only
#  - /GS /sdl, Control Flow Guard, CET shadow stack compatible, ASLR
#    (64-bit high entropy), DEP
#  - /Qspectre only when the Spectre-mitigated libraries are installed
# ---------------------------------------------------------------------------
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
if(MSVC)
    add_compile_options(
        "$<$<COMPILE_LANGUAGE:CXX>:/GS>"
        "$<$<COMPILE_LANGUAGE:CXX>:/sdl>"
        "$<$<COMPILE_LANGUAGE:CXX>:/guard:cf>"
    )
    add_link_options(
        /guard:cf
        /CETCOMPAT
        /DYNAMICBASE
        /NXCOMPAT
        /HIGHENTROPYVA
        /DEPENDENTLOADFLAG:0x800
    )
    # Spectre-mitigated CRT/STL: <VCToolsInstallDir>\lib\spectre\x64
    set(NDM_SPECTRE_LIBDIR "")
    if(DEFINED ENV{VCToolsInstallDir})
        file(TO_CMAKE_PATH "$ENV{VCToolsInstallDir}" _ndm_vctools)
        if(EXISTS "${_ndm_vctools}/lib/spectre/x64")
            set(NDM_SPECTRE_LIBDIR "${_ndm_vctools}/lib/spectre/x64")
        endif()
    endif()
    if(NDM_SPECTRE_LIBDIR)
        message(STATUS "NDM: Spectre-mitigated libraries found, building with /Qspectre")
        add_compile_options("$<$<COMPILE_LANGUAGE:CXX>:/Qspectre>")
        add_link_options("/LIBPATH:${NDM_SPECTRE_LIBDIR}")
    else()
        message(STATUS "NDM: Spectre-mitigated libraries NOT installed (MSVC component "
                       "'MSVC v143 - x64/x86 Spectre-mitigated libs'): building without /Qspectre")
    endif()
endif()

# Delay-load helper for the installer / uninstaller: every DLL that is not a
# KnownDLL is delay-loaded, so it is only loaded after WinMain has called
# SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32).
function(ndm_delay_load target)
    foreach(dll IN LISTS ARGN)
        target_link_options(${target} PRIVATE "/DELAYLOAD:${dll}")
    endforeach()
    target_link_libraries(${target} PRIVATE delayimp)
endfunction()

# ---------------------------------------------------------------------------
# Output directories
# ---------------------------------------------------------------------------
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")

# ---------------------------------------------------------------------------
# Common definitions
# ---------------------------------------------------------------------------
add_compile_definitions(
    UNICODE
    _UNICODE
    WIN32
    _WINDOWS
    NOMINMAX
    _CRT_SECURE_NO_WARNINGS
)

# ---------------------------------------------------------------------------
# ndm_common — shared types and helpers
# ---------------------------------------------------------------------------
add_library(ndm_common STATIC
    src/common/ndm_common.cpp
    src/common/ndm_common.h
)
target_include_directories(ndm_common PUBLIC src/common)

# ---------------------------------------------------------------------------
# ndm_network — WinHTTP-based segmented HTTP engine
# ---------------------------------------------------------------------------
add_library(ndm_network STATIC
    src/network/ndm_http.cpp
    src/network/ndm_http.h
)
target_include_directories(ndm_network PUBLIC src/network)
target_link_libraries(ndm_network PUBLIC ndm_common winhttp)

# ---------------------------------------------------------------------------
# ndm_core — queue, config, scheduler
# ---------------------------------------------------------------------------
add_library(ndm_core STATIC
    src/core/ndm_queue.cpp
    src/core/ndm_queue.h
    src/core/ndm_config.cpp
    src/core/ndm_config.h
    src/scheduler/ndm_scheduler.cpp
    src/scheduler/ndm_scheduler.h
    src/media/ndm_media.cpp
    src/media/ndm_media.h
    src/torrent/ndm_torrent.cpp
    src/torrent/ndm_torrent.h
    src/torrent/ndm_torrent_internal.h
)
target_include_directories(ndm_core PUBLIC src/core src/scheduler)
target_link_libraries(ndm_core PUBLIC ndm_common ndm_network advapi32 shell32)

# ---------------------------------------------------------------------------
# ndm_msghost_lib — IPC bridge (embedded in NDMan)
# ---------------------------------------------------------------------------
add_library(ndm_msghost_lib STATIC
    src/msghost/ndm_msghost.cpp
    src/msghost/ndm_msghost.h
)
target_include_directories(ndm_msghost_lib PUBLIC src/msghost)
target_link_libraries(ndm_msghost_lib PUBLIC ndm_common)

# ---------------------------------------------------------------------------
# NDMan.exe — main application
# ---------------------------------------------------------------------------
add_executable(NDMan WIN32
    src/core/NDMan.cpp
    src/ui/ndm_mainwnd.cpp
    src/ui/ndm_mainwnd.h
    src/ui/ndm_adddlg.cpp
    src/ui/ndm_adddlg.h
    src/ui/ndm_optdlg.cpp
    src/ui/ndm_optdlg.h
    src/ui/ndm_progdlg.cpp
    src/ui/ndm_progdlg.h
    src/ui/ndm_theme.cpp
    src/ui/ndm_theme.h
    src/ui/ndm_firstrun.cpp
    src/ui/ndm_firstrun.h
    src/ui/ndm_uilogic.cpp
    src/ui/ndm_uilogic.h
    src/ui/ndm_dialogs.cpp
    src/ui/ndm_dialogs.h
    src/ui/ndm_power.cpp
    src/ui/ndm_power.h
    src/ui/ndm_scheddlg.cpp
    src/ui/ndm_scheddlg.h
    src/ui/ndm_resource.h
    src/torrent/ndm_torrent_dlg.cpp   # torrent add dialog (needs ui theme helpers)
    resources/NDMan.rc
    resources/NDMan.manifest          # Common-Controls v6 + PerMonitorV2 DPI
)

target_include_directories(NDMan PRIVATE
    src
    src/common
    src/core
    src/network
    src/ui
    src/msghost
    src/scheduler
)

target_link_libraries(NDMan PRIVATE
    ndm_common
    ndm_network
    ndm_core
    ndm_msghost_lib
    comctl32
    shell32
    user32
    advapi32
    shlwapi
    ole32
    uuid
    comdlg32
    msimg32
    uxtheme
    gdi32
    gdiplus
    dwmapi
    winmm
    powrprof
)

# UI sources contain UTF-8 literals (em dash, middle dot, ...)
target_compile_options(NDMan PRIVATE /utf-8)

set_target_properties(NDMan PROPERTIES
    OUTPUT_NAME "NDMan"
    WIN32_EXECUTABLE TRUE
)

# ---------------------------------------------------------------------------
# ndmsetup643build1.exe — installer
# ---------------------------------------------------------------------------
add_executable(ndmsetup WIN32
    src/installer/ndmsetup.cpp
    src/installer/ndm_sfx.h           # SFX v2 payload format
    src/installer/sfx_extract.h       # streaming extractor (cabinet.dll)
    src/installer/ndm_procs.h
    src/installer/ndm_fsutil.h        # no-follow delete, ACL / owner / reparse checks
    src/installer/ndm_userinfo.h      # interactive user SID (recorded for the uninstaller)
    src/installer/ndmsetup.rc         # icon (resources/icons/ndm.ico) + version info
    src/installer/ndmsetup.manifest   # requireAdministrator (needed for Program Files / HKLM), PMv2 DPI
)
# Our manifest already carries trustInfo; stop the linker adding its default asInvoker one.
target_link_options(ndmsetup PRIVATE /MANIFESTUAC:NO)
target_include_directories(ndmsetup PRIVATE src/common)
target_link_libraries(ndmsetup PRIVATE
    ndm_common
    comctl32
    shell32
    user32
    advapi32
    kernel32
    ole32
    oleaut32
    uuid
    shlwapi
    msimg32
    uxtheme
    gdiplus
    dwmapi
    bcrypt        # payload SHA-256, random names
    cabinet       # Windows Compression API (LZMS / XPRESS_HUFF payload)
    wtsapi32      # interactive user lookup (ndm_userinfo.h)
)
# Setup usually runs from Downloads: no non-KnownDLL may be a static import.
ndm_delay_load(ndmsetup
    cabinet.dll comctl32.dll uxtheme.dll msimg32.dll dwmapi.dll gdiplus.dll bcrypt.dll wtsapi32.dll)
set_target_properties(ndmsetup PROPERTIES
    OUTPUT_NAME "ndmsetup643build1"
    WIN32_EXECUTABLE TRUE
)

# ---------------------------------------------------------------------------
# Uninstall.exe — uninstaller
# ---------------------------------------------------------------------------
add_executable(Uninstall WIN32
    src/uninstaller/Uninstall.cpp
    src/uninstaller/ndm_uninst_logic.h  # arguments, per-user data removal (unit-tested)
    src/uninstaller/Uninstall.rc
)
target_link_libraries(Uninstall PRIVATE
    user32
    advapi32
    shell32
    ole32
    shlwapi
    bcrypt        # random stage-2 folder name
    wtsapi32      # interactive user lookup (ndm_userinfo.h)
)
# The stage-2 copy runs from a temp folder: KnownDLLs only as static imports.
ndm_delay_load(Uninstall bcrypt.dll wtsapi32.dll)
# Uninstaller removes Program Files content and HKLM keys: needs admin.
target_link_options(Uninstall PRIVATE "/MANIFESTUAC:level='requireAdministrator' uiAccess='false'")
set_target_properties(Uninstall PROPERTIES
    WIN32_EXECUTABLE TRUE
)

# ---------------------------------------------------------------------------
# NDMShellExt.dll — shell context menu extension (x64, loaded by Explorer).
# Setup writes its COM keys directly; DllRegisterServer is kept for dev use.
# ---------------------------------------------------------------------------
add_library(NDMShellExt SHARED
    src/shellext/NDMShellExt.cpp
    src/shellext/NDMShellExt.def      # export COM entry points so regsvr32 works
)
target_link_libraries(NDMShellExt PRIVATE
    user32
    advapi32
    shell32
    shlwapi
    ole32
    uuid
)
set_target_properties(NDMShellExt PROPERTIES
    PREFIX ""
)

# ---------------------------------------------------------------------------
# NDMMsgHost.exe — standalone Chrome native messaging relay
# ---------------------------------------------------------------------------
add_executable(NDMMsgHost
    src/msghost/ndm_msghost.cpp
)
target_include_directories(NDMMsgHost PRIVATE src/common)
target_compile_definitions(NDMMsgHost PRIVATE NDM_MSGHOST_STANDALONE=1)
target_link_libraries(NDMMsgHost PRIVATE
    ndm_common
    user32
    advapi32
)
set_target_properties(NDMMsgHost PROPERTIES
    WIN32_EXECUTABLE FALSE   # console subsystem for stdin/stdout
)

# ---------------------------------------------------------------------------
# Install rules (optional — copy to dist/ via build.bat)
# ---------------------------------------------------------------------------
install(TARGETS NDMan ndmsetup Uninstall NDMShellExt NDMMsgHost
    RUNTIME DESTINATION dist
    LIBRARY DESTINATION dist
)
