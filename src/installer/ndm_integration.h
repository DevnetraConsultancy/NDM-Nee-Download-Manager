#pragma once
// ---------------------------------------------------------------------------
// System integration shared by the installer (register) and the uninstaller
// (unregister), so both always agree on key names:
//   - magnet: links and .torrent files (ProgIDs + RegisteredApplications /
//     Capabilities so Windows "Default apps" lists NDM)
//   - browser extension auto-install (Chrome/Edge external extension keys,
//     Firefox enterprise policy) — inactive until the IDs below are set
// ---------------------------------------------------------------------------
#include <windows.h>
#include <shlobj.h>
#include <string>
#include <vector>
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include "ndm_json.h"

// ===== Publishing constants (edit when the extensions are published) ======
// Chrome Web Store extension ID. Empty = no Chrome auto-install.
#define NDM_CWS_EXTENSION_ID          ""
// Microsoft Edge Add-ons extension ID. Empty = no Edge auto-install.
#define NDM_EDGE_EXTENSION_ID         ""
// Set to true once ext\ndm.xpi is a Mozilla-signed build.
static const bool FIREFOX_SIGNED_XPI_AVAILABLE = false;
// Read through a function so the prepared Firefox-policy code below is not a
// "constant condition" (/W4 C4127) while the switch is off.
static inline bool NdmFirefoxSignedXpiAvailable() { return FIREFOX_SIGNED_XPI_AVAILABLE; }
// Uninstall entry "URLInfoAbout"; empty = not written.
#define NDM_URL_INFO_ABOUT            L""
// ===========================================================================

#define NDM_UNPACKED_EXTENSION_ID     "dbphbdbafcniaifdaeikngfgmdmeeobf"  // keyed unpacked build
#define NDM_FIREFOX_ADDON_ID          "ndm@devnetra.com"

#define NDM_CAPS_KEY      L"Software\\Devnetra\\NDM\\Capabilities"
#define NDM_REGAPP_NAME   L"Nee Download Manager"
#define NDM_PROGID_TORRENT L"NDM.Torrent"
#define NDM_PROGID_MAGNET  L"NDM.Magnet"
#define NDM_OWNER_VALUE    L"NDM.Owner"     // marks keys NDM created as default handler

static inline std::wstring NdmWiden(const char* s)
{
    std::wstring w;
    while (*s) w += (wchar_t)(unsigned char)*s++;
    return w;
}

static inline bool NdmRegSetStr(HKEY root, const std::wstring& key, const wchar_t* name,
                         const std::wstring& val, REGSAM view = 0)
{
    HKEY h = nullptr;
    if (RegCreateKeyExW(root, key.c_str(), 0, nullptr, 0, KEY_WRITE | view, nullptr, &h, nullptr) != ERROR_SUCCESS)
        return false;
    LSTATUS ls = RegSetValueExW(h, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(val.c_str()),
                                (DWORD)((val.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(h);
    return ls == ERROR_SUCCESS;
}

static inline std::wstring NdmRegGetStr(HKEY root, const std::wstring& key, const wchar_t* name, REGSAM view = 0)
{
    HKEY h = nullptr;
    if (RegOpenKeyExW(root, key.c_str(), 0, KEY_READ | view, &h) != ERROR_SUCCESS) return L"";
    wchar_t buf[2048] = {};
    DWORD sz = sizeof(buf) - sizeof(wchar_t), type = 0;
    LSTATUS ls = RegQueryValueExW(h, name, nullptr, &type, reinterpret_cast<BYTE*>(buf), &sz);
    RegCloseKey(h);
    if (ls != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ)) return L"";
    return buf;
}

static inline bool NdmRegKeyExists(HKEY root, const std::wstring& key, REGSAM view = 0)
{
    HKEY h = nullptr;
    if (RegOpenKeyExW(root, key.c_str(), 0, KEY_READ | view, &h) != ERROR_SUCCESS) return false;
    RegCloseKey(h);
    return true;
}

static inline bool NdmRegValueExists(HKEY root, const std::wstring& key, const wchar_t* name, REGSAM view = 0)
{
    HKEY h = nullptr;
    if (RegOpenKeyExW(root, key.c_str(), 0, KEY_READ | view, &h) != ERROR_SUCCESS) return false;
    LSTATUS ls = RegQueryValueExW(h, name, nullptr, nullptr, nullptr, nullptr);
    RegCloseKey(h);
    return ls == ERROR_SUCCESS;
}

static inline void NdmRegDeleteTree(HKEY root, const std::wstring& key, REGSAM view = 0)
{
    // RegDeleteTree has no view flag: open the parent in the wanted view.
    size_t s = key.rfind(L'\\');
    std::wstring parent = s == std::wstring::npos ? L"" : key.substr(0, s);
    std::wstring leaf   = s == std::wstring::npos ? key : key.substr(s + 1);
    HKEY h = nullptr;
    if (RegOpenKeyExW(root, parent.c_str(), 0, KEY_ALL_ACCESS | view, &h) != ERROR_SUCCESS) return;
    RegDeleteTreeW(h, leaf.c_str());
    RegDeleteKeyExW(h, leaf.c_str(), view, 0);
    RegCloseKey(h);
}

static inline void NdmRegDeleteValue(HKEY root, const std::wstring& key, const wchar_t* name, REGSAM view = 0)
{
    HKEY h = nullptr;
    if (RegOpenKeyExW(root, key.c_str(), 0, KEY_SET_VALUE | view, &h) != ERROR_SUCCESS) return;
    RegDeleteValueW(h, name);
    RegCloseKey(h);
}

static inline bool NdmIsWow64Capable()
{
    BOOL wow = FALSE;
    IsWow64Process(GetCurrentProcess(), &wow);
    return wow != FALSE;
}

// ---------------------------------------------------------------------------
// Shell extension (NDMShellExt.dll) COM + context-menu handler registration.
// Written directly by setup instead of regsvr32, so no elevated process ever
// loads the DLL. Mirrors DllRegisterServer in src/shellext/NDMShellExt.cpp
// exactly (HKCR == HKLM\Software\Classes for an elevated writer).
// ---------------------------------------------------------------------------
#define NDM_SHELLEXT_CLSID  L"{A1B2C3D4-E5F6-7890-ABCD-EF1234567890}"   // CLSID_NDMShellExt
#define NDM_SHELLEXT_NAME   L"NDMShellExt"

// "Download with NDM" is offered for .torrent and .url files only.
static const wchar_t* const kNdmShellExtHandlerKeys[] = {
    L"Software\\Classes\\SystemFileAssociations\\.torrent\\shellex\\ContextMenuHandlers\\" NDM_SHELLEXT_NAME,
    L"Software\\Classes\\SystemFileAssociations\\.url\\shellex\\ContextMenuHandlers\\" NDM_SHELLEXT_NAME,
};
// Older builds registered the menu for every file type ("*").
static const wchar_t* const kNdmShellExtLegacyHandlerKeys[] = {
    L"Software\\Classes\\*\\shellex\\ContextMenuHandlers\\" NDM_SHELLEXT_NAME,
};

// Remove a handler key only while it still points at our CLSID.
static inline void NdmDeleteOwnShellExtHandler(const wchar_t* k, REGSAM view)
{
    std::wstring cur = NdmRegGetStr(HKEY_LOCAL_MACHINE, k, nullptr, view);
    if (NdmRegKeyExists(HKEY_LOCAL_MACHINE, k, view) &&
        (cur.empty() || _wcsicmp(cur.c_str(), NDM_SHELLEXT_CLSID) == 0))
        NdmRegDeleteTree(HKEY_LOCAL_MACHINE, k, view);
}

static inline bool NdmRegisterShellExt(const std::wstring& dllPath)
{
    const REGSAM v = KEY_WOW64_64KEY;
    const std::wstring clsid = std::wstring(L"Software\\Classes\\CLSID\\") + NDM_SHELLEXT_CLSID;
    bool ok = NdmRegSetStr(HKEY_LOCAL_MACHINE, clsid, nullptr, L"NDM Shell Extension", v);
    ok &= NdmRegSetStr(HKEY_LOCAL_MACHINE, clsid + L"\\InprocServer32", nullptr, dllPath, v);
    ok &= NdmRegSetStr(HKEY_LOCAL_MACHINE, clsid + L"\\InprocServer32", L"ThreadingModel", L"Apartment", v);
    for (const wchar_t* k : kNdmShellExtHandlerKeys)
        ok &= NdmRegSetStr(HKEY_LOCAL_MACHINE, k, nullptr, NDM_SHELLEXT_CLSID, v);
    for (const wchar_t* k : kNdmShellExtLegacyHandlerKeys)   // upgrade: drop the all-files menu
        NdmDeleteOwnShellExtHandler(k, v);
    return ok;
}

// view: KEY_WOW64_64KEY (current x64 builds) or KEY_WOW64_32KEY (old x86 builds).
static inline void NdmUnregisterShellExt(REGSAM view)
{
    for (const wchar_t* k : kNdmShellExtHandlerKeys) NdmDeleteOwnShellExtHandler(k, view);
    for (const wchar_t* k : kNdmShellExtLegacyHandlerKeys) NdmDeleteOwnShellExtHandler(k, view);
    NdmRegDeleteTree(HKEY_LOCAL_MACHINE, std::wstring(L"Software\\Classes\\CLSID\\") + NDM_SHELLEXT_CLSID, view);
}

// ---------------------------------------------------------------------------
// magnet: + .torrent
// ---------------------------------------------------------------------------
static const wchar_t* const kClasses = L"Software\\Classes\\";

// True if some other application already handles the scheme / extension.
static inline bool NdmMagnetOwnedByOther()
{
    const HKEY roots[] = { HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE };
    for (HKEY r : roots) {
        std::wstring cmd = NdmRegGetStr(r, std::wstring(kClasses) + L"magnet\\shell\\open\\command", nullptr);
        if (!cmd.empty() && !wcsstr(cmd.c_str(), L"NDMan.exe")) return true;
    }
    std::wstring uc = NdmRegGetStr(HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\Shell\\Associations\\UrlAssociations\\magnet\\UserChoice", L"ProgId");
    return !uc.empty() && uc != NDM_PROGID_MAGNET;
}

static inline bool NdmTorrentOwnedByOther()
{
    const HKEY roots[] = { HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE };
    for (HKEY r : roots) {
        std::wstring pid = NdmRegGetStr(r, std::wstring(kClasses) + L".torrent", nullptr);
        if (!pid.empty() && pid != NDM_PROGID_TORRENT) return true;
    }
    std::wstring uc = NdmRegGetStr(HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\FileExts\\.torrent\\UserChoice", L"ProgId");
    return !uc.empty() && uc != NDM_PROGID_TORRENT;
}

static inline void NdmRegisterProtocolKey(const std::wstring& key, const std::wstring& exe, const wchar_t* desc)
{
    std::wstring open = L"\"" + exe + L"\" \"%1\"";
    NdmRegSetStr(HKEY_LOCAL_MACHINE, key, nullptr, desc);
    NdmRegSetStr(HKEY_LOCAL_MACHINE, key, L"URL Protocol", L"");
    NdmRegSetStr(HKEY_LOCAL_MACHINE, key + L"\\DefaultIcon", nullptr, L"\"" + exe + L"\",0");
    NdmRegSetStr(HKEY_LOCAL_MACHINE, key + L"\\shell\\open\\command", nullptr, open);
}

static inline void NdmRegisterAssociations(const std::wstring& installDir)
{
    const std::wstring exe  = installDir + L"\\NDMan.exe";
    const std::wstring icon = L"\"" + exe + L"\",0";
    const std::wstring open = L"\"" + exe + L"\" \"%1\"";
    const std::wstring C = kClasses;

    // ProgIDs
    NdmRegSetStr(HKEY_LOCAL_MACHINE, C + NDM_PROGID_TORRENT, nullptr, L"Torrent file");
    NdmRegSetStr(HKEY_LOCAL_MACHINE, C + NDM_PROGID_TORRENT, L"FriendlyTypeName", L"Torrent file");
    NdmRegSetStr(HKEY_LOCAL_MACHINE, C + NDM_PROGID_TORRENT + L"\\DefaultIcon", nullptr, icon);
    NdmRegSetStr(HKEY_LOCAL_MACHINE, C + NDM_PROGID_TORRENT + L"\\shell\\open\\command", nullptr, open);
    NdmRegisterProtocolKey(C + NDM_PROGID_MAGNET, exe, L"URL:Magnet Link");
    NdmRegSetStr(HKEY_LOCAL_MACHINE, C + L".torrent\\OpenWithProgids", NDM_PROGID_TORRENT, L"");
    NdmRegSetStr(HKEY_LOCAL_MACHINE, C + L"Applications\\NDMan.exe\\shell\\open\\command", nullptr, open);
    NdmRegSetStr(HKEY_LOCAL_MACHINE, C + L"Applications\\NDMan.exe\\SupportedTypes", L".torrent", L"");

    // Default handler only when nobody else owns it yet
    if (!NdmMagnetOwnedByOther()) {
        NdmRegisterProtocolKey(C + L"magnet", exe, L"URL:Magnet Link");
        NdmRegSetStr(HKEY_LOCAL_MACHINE, C + L"magnet", NDM_OWNER_VALUE, L"1");
    }
    if (!NdmTorrentOwnedByOther()) {
        NdmRegSetStr(HKEY_LOCAL_MACHINE, C + L".torrent", nullptr, NDM_PROGID_TORRENT);
        NdmRegSetStr(HKEY_LOCAL_MACHINE, C + L".torrent", L"Content Type", L"application/x-bittorrent");
        NdmRegSetStr(HKEY_LOCAL_MACHINE, C + L".torrent", NDM_OWNER_VALUE, L"1");
    }

    // Default Programs / "Default apps" registration (native 64-bit view)
    const REGSAM v = KEY_WOW64_64KEY;
    NdmRegSetStr(HKEY_LOCAL_MACHINE, NDM_CAPS_KEY, L"ApplicationName", L"Nee Download Manager", v);
    NdmRegSetStr(HKEY_LOCAL_MACHINE, NDM_CAPS_KEY, L"ApplicationDescription",
                 L"Download manager for files, videos, magnet links and torrents.", v);
    NdmRegSetStr(HKEY_LOCAL_MACHINE, NDM_CAPS_KEY, L"ApplicationIcon", icon, v);
    NdmRegSetStr(HKEY_LOCAL_MACHINE, std::wstring(NDM_CAPS_KEY) + L"\\URLAssociations", L"magnet", NDM_PROGID_MAGNET, v);
    NdmRegSetStr(HKEY_LOCAL_MACHINE, std::wstring(NDM_CAPS_KEY) + L"\\FileAssociations", L".torrent", NDM_PROGID_TORRENT, v);
    NdmRegSetStr(HKEY_LOCAL_MACHINE, L"Software\\RegisteredApplications", NDM_REGAPP_NAME, NDM_CAPS_KEY, v);

    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
}

static inline void NdmUnregisterAssociations()
{
    const std::wstring C = kClasses;
    NdmRegDeleteTree(HKEY_LOCAL_MACHINE, C + NDM_PROGID_TORRENT);
    NdmRegDeleteTree(HKEY_LOCAL_MACHINE, C + NDM_PROGID_MAGNET);
    NdmRegDeleteTree(HKEY_LOCAL_MACHINE, C + L"Applications\\NDMan.exe");
    NdmRegDeleteValue(HKEY_LOCAL_MACHINE, C + L".torrent\\OpenWithProgids", NDM_PROGID_TORRENT);

    // magnet: only if NDM created it (or it still points at NDMan.exe)
    std::wstring mcmd = NdmRegGetStr(HKEY_LOCAL_MACHINE, C + L"magnet\\shell\\open\\command", nullptr);
    if (NdmRegValueExists(HKEY_LOCAL_MACHINE, C + L"magnet", NDM_OWNER_VALUE) ||
        (!mcmd.empty() && wcsstr(mcmd.c_str(), L"NDMan.exe")))
        NdmRegDeleteTree(HKEY_LOCAL_MACHINE, C + L"magnet");

    // .torrent: drop our default, keep the key if others registered under it
    if (NdmRegGetStr(HKEY_LOCAL_MACHINE, C + L".torrent", nullptr) == NDM_PROGID_TORRENT) {
        NdmRegDeleteValue(HKEY_LOCAL_MACHINE, C + L".torrent", nullptr);
        if (NdmRegValueExists(HKEY_LOCAL_MACHINE, C + L".torrent", NDM_OWNER_VALUE))
            NdmRegDeleteValue(HKEY_LOCAL_MACHINE, C + L".torrent", L"Content Type");
    }
    NdmRegDeleteValue(HKEY_LOCAL_MACHINE, C + L".torrent", NDM_OWNER_VALUE);
    {
        HKEY h = nullptr;   // remove .torrent entirely if nothing else is left in it
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, (C + L".torrent").c_str(), 0, KEY_READ, &h) == ERROR_SUCCESS) {
            DWORD sub = 0, vals = 0;
            RegQueryInfoKeyW(h, nullptr, nullptr, nullptr, &sub, nullptr, nullptr, &vals, nullptr, nullptr, nullptr, nullptr);
            RegCloseKey(h);
            if (sub == 1 && vals == 0) {
                HKEY o = nullptr; DWORD ov = 1;
                if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, (C + L".torrent\\OpenWithProgids").c_str(), 0, KEY_READ, &o) == ERROR_SUCCESS) {
                    RegQueryInfoKeyW(o, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, &ov, nullptr, nullptr, nullptr, nullptr);
                    RegCloseKey(o);
                }
                if (ov == 0) NdmRegDeleteTree(HKEY_LOCAL_MACHINE, C + L".torrent");
            } else if (sub == 0 && vals == 0) {
                NdmRegDeleteTree(HKEY_LOCAL_MACHINE, C + L".torrent");
            }
        }
    }

    const REGSAM views[] = { KEY_WOW64_64KEY, KEY_WOW64_32KEY };
    for (REGSAM v : views) {
        NdmRegDeleteValue(HKEY_LOCAL_MACHINE, L"Software\\RegisteredApplications", NDM_REGAPP_NAME, v);
        NdmRegDeleteTree(HKEY_LOCAL_MACHINE, NDM_CAPS_KEY, v);
    }
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
}

// ---------------------------------------------------------------------------
// Chrome / Edge external extension (store) keys — both registry views
// ---------------------------------------------------------------------------
static inline void NdmRegisterStoreExtensions()
{
    const REGSAM views[] = { KEY_WOW64_64KEY, KEY_WOW64_32KEY };
    for (REGSAM v : views) {
        if (NDM_CWS_EXTENSION_ID[0])
            NdmRegSetStr(HKEY_LOCAL_MACHINE,
                         L"Software\\Google\\Chrome\\Extensions\\" + NdmWiden(NDM_CWS_EXTENSION_ID),
                         L"update_url", L"https://clients2.google.com/service/update2/crx", v);
        if (NDM_EDGE_EXTENSION_ID[0])
            NdmRegSetStr(HKEY_LOCAL_MACHINE,
                         L"Software\\Microsoft\\Edge\\Extensions\\" + NdmWiden(NDM_EDGE_EXTENSION_ID),
                         L"update_url", L"https://edge.microsoft.com/extensionwebstorebase/v1/crx", v);
    }
}

static inline void NdmUnregisterStoreExtensions()
{
    const REGSAM views[] = { KEY_WOW64_64KEY, KEY_WOW64_32KEY };
    for (REGSAM v : views) {
        if (NDM_CWS_EXTENSION_ID[0])
            NdmRegDeleteTree(HKEY_LOCAL_MACHINE,
                             L"Software\\Google\\Chrome\\Extensions\\" + NdmWiden(NDM_CWS_EXTENSION_ID), v);
        if (NDM_EDGE_EXTENSION_ID[0])
            NdmRegDeleteTree(HKEY_LOCAL_MACHINE,
                             L"Software\\Microsoft\\Edge\\Extensions\\" + NdmWiden(NDM_EDGE_EXTENSION_ID), v);
    }
}

// Chrome-family allowed_origins entries for the native messaging manifest.
static inline std::vector<std::string> NdmChromeExtensionIds()
{
    std::vector<std::string> ids = { NDM_UNPACKED_EXTENSION_ID };
    for (const char* id : { NDM_CWS_EXTENSION_ID, NDM_EDGE_EXTENSION_ID })
        if (id[0] && std::find(ids.begin(), ids.end(), std::string(id)) == ids.end()) ids.push_back(id);
    return ids;
}

// ---------------------------------------------------------------------------
// Firefox enterprise policy (distribution\policies.json), merged in place
// ---------------------------------------------------------------------------
static inline std::vector<std::wstring> NdmFindFirefoxDirs()
{
    std::vector<std::wstring> dirs;
    auto add = [&](std::wstring d) {
        while (!d.empty() && (d.back() == L'\\' || d.back() == L'/')) d.pop_back();
        if (d.empty()) return;
        DWORD a = GetFileAttributesW((d + L"\\firefox.exe").c_str());
        if (a == INVALID_FILE_ATTRIBUTES || (a & FILE_ATTRIBUTE_DIRECTORY)) return;
        for (auto& x : dirs) if (_wcsicmp(x.c_str(), d.c_str()) == 0) return;
        dirs.push_back(d);
    };
    const wchar_t* products[] = { L"Mozilla Firefox", L"Mozilla Firefox ESR", L"Firefox Developer Edition",
                                  L"Firefox Nightly" };
    const REGSAM views[] = { KEY_WOW64_64KEY, KEY_WOW64_32KEY };
    for (REGSAM v : views) for (const wchar_t* prod : products) {
        std::wstring base = std::wstring(L"Software\\Mozilla\\") + prod;
        HKEY h = nullptr;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, base.c_str(), 0, KEY_READ | v, &h) != ERROR_SUCCESS) continue;
        wchar_t name[256];
        for (DWORD i = 0;; i++) {
            DWORD n = 256;
            if (RegEnumKeyExW(h, i, name, &n, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
            add(NdmRegGetStr(HKEY_LOCAL_MACHINE, base + L"\\" + name + L"\\Main", L"Install Directory", v));
        }
        RegCloseKey(h);
    }
    wchar_t buf[MAX_PATH];
    const wchar_t* envs[] = { L"%ProgramW6432%", L"%ProgramFiles%", L"%ProgramFiles(x86)%" };
    for (const wchar_t* e : envs) {
        if (ExpandEnvironmentStringsW(e, buf, MAX_PATH) && buf[0] != L'%') {
            add(std::wstring(buf) + L"\\Mozilla Firefox");
            add(std::wstring(buf) + L"\\Firefox Developer Edition");
        }
    }
    return dirs;
}

static inline bool NdmReadFileUtf8(const std::wstring& path, std::string& out)
{
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz = {};
    GetFileSizeEx(h, &sz);
    if (sz.QuadPart > 16 * 1024 * 1024) { CloseHandle(h); return false; }
    out.resize((size_t)sz.QuadPart);
    DWORD rd = 0;
    BOOL ok = out.empty() || ReadFile(h, &out[0], (DWORD)out.size(), &rd, nullptr);
    CloseHandle(h);
    return ok && rd == out.size();
}

static inline bool NdmWriteFileUtf8(const std::wstring& path, const std::string& data)
{
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD wr = 0;
    BOOL ok = WriteFile(h, data.data(), (DWORD)data.size(), &wr, nullptr);
    CloseHandle(h);
    return ok && wr == data.size();
}

static inline std::string NdmUtf8(const std::wstring& w)
{
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}

static inline std::string NdmFileUrl(const std::wstring& path)
{
    std::string u = "file:///";
    for (unsigned char c : NdmUtf8(path)) {
        if (c == '\\') u += '/';
        else if (isalnum(c) || strchr("/-_.~:", c)) u += (char)c;
        else { char b[4]; snprintf(b, sizeof(b), "%%%02X", c); u += b; }
    }
    return u;
}

// add == true: install entry; false: remove it. Never clobbers other policies;
// an unparsable existing file is left untouched.
static inline int NdmUpdateFirefoxPolicies(const std::wstring& installDir, bool add)
{
    int changed = 0;
    std::wstring xpi = installDir + L"\\ext\\ndm.xpi";
    if (add) {
        DWORD a = GetFileAttributesW(xpi.c_str());
        if (!NdmFirefoxSignedXpiAvailable() || a == INVALID_FILE_ATTRIBUTES || (a & FILE_ATTRIBUTE_DIRECTORY))
            return 0;
    }
    for (const std::wstring& dir : NdmFindFirefoxDirs()) {
        std::wstring distDir = dir + L"\\distribution";
        std::wstring file = distDir + L"\\policies.json";
        std::string text;
        NdmJson root = NdmJson::MakeObject();
        bool existed = NdmReadFileUtf8(file, text);
        if (existed && !text.empty() && (!NdmJson::Parse(text, root) || root.type != NdmJson::Object))
            continue;
        if (!add && !existed) continue;

        NdmJson* pol = root.Find("policies");
        if (!pol || pol->type != NdmJson::Object) {
            if (!add) continue;
            pol = &root.Set("policies", NdmJson::MakeObject());
        }
        NdmJson* es = pol->Find("ExtensionSettings");
        if (add) {
            if (!es || es->type != NdmJson::Object) es = &pol->Set("ExtensionSettings", NdmJson::MakeObject());
            NdmJson e = NdmJson::MakeObject();
            e.Set("installation_mode", NdmJson::MakeString("normal_installed"));
            e.Set("install_url", NdmJson::MakeString(NdmFileUrl(xpi)));
            es->Set(NDM_FIREFOX_ADDON_ID, std::move(e));
            CreateDirectoryW(distDir.c_str(), nullptr);
            if (NdmWriteFileUtf8(file, root.Dump())) changed++;
        } else {
            if (!es || !es->Erase(NDM_FIREFOX_ADDON_ID)) continue;
            if (es->obj.empty()) pol->Erase("ExtensionSettings");
            if (pol->obj.empty() && root.obj.size() == 1) {
                if (DeleteFileW(file.c_str())) { RemoveDirectoryW(distDir.c_str()); changed++; }
            } else if (NdmWriteFileUtf8(file, root.Dump())) {
                changed++;
            }
        }
    }
    return changed;
}
