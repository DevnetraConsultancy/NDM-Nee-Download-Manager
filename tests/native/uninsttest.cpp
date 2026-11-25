// Unit tests for the uninstaller / setup logic that can run unelevated:
//   uninsttest.exe <scratch dir>
// argument parsing, previous-install detection, SID / profile helpers,
// per-user data removal on a fake profile (allow-list, junction refusal,
// profile guard), no-follow registry tree removal under a private HKCU test
// key (never HKCU\Software\Devnetra), interactive-user detection.
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <cstdio>
#include <string>
#include <vector>
#include "../../src/uninstaller/ndm_uninst_logic.h"
#include "../../src/installer/ndm_userinfo.h"

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")

static int g_fail = 0, g_pass = 0;
#define CHECK(c, ...) do { if (c) { ++g_pass; } else { ++g_fail; wprintf(L"FAIL %d: ", __LINE__); wprintf(__VA_ARGS__); wprintf(L"\n"); } } while (0)

static NdmUninstArgs Parse(const std::wstring& cmdLine)
{
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(cmdLine.c_str(), &argc);
    NdmUninstArgs a = NdmParseUninstallArgs(argc, argv);
    LocalFree(argv);
    return a;
}

static void TestArgs()
{
    NdmUninstArgs a = Parse(L"\"C:\\P F\\NDM\\Uninstall.exe\"");
    CHECK(!a.stage2 && !a.silent && !a.removeData, L"no args = interactive");
    a = Parse(L"Uninstall.exe /S");
    CHECK(a.silent && !a.removeData && !a.stage2, L"/S");
    a = Parse(L"Uninstall.exe /s");
    CHECK(a.silent, L"/s lower case");
    a = Parse(L"Uninstall.exe /quiet");
    CHECK(a.silent, L"/quiet");
    a = Parse(L"Uninstall.exe -QUIET /RemoveData");
    CHECK(a.silent && a.removeData, L"-QUIET /RemoveData");
    a = Parse(L"Uninstall.exe /removedata");
    CHECK(!a.silent && a.removeData, L"/removedata alone (interactive, preselected)");
    a = Parse(L"Uninstall.exe /Sx /silentx S removedata");
    CHECK(!a.silent && !a.removeData, L"near misses are ignored");
    a = Parse(L"Uninstall.exe /stage2 \"C:\\P F\\NDM\" 1234");
    CHECK(a.stage2 && a.stage2Dir == L"C:\\P F\\NDM" && a.parentPid == 1234 && !a.silent, L"stage2 basic");
    a = Parse(L"Uninstall.exe /stage2 \"C:\\P F\\NDM\" 12x4 /S");
    CHECK(!a.stage2 && a.silent, L"stage2 with a bad pid is not stage 2");
    a = Parse(L"Uninstall.exe /stage2 \"C:\\X\"");
    CHECK(!a.stage2, L"stage2 needs dir + pid");
    a = Parse(L"Uninstall.exe /S /stage2 \"C:\\X\" 5");
    CHECK(!a.stage2 && a.silent, L"/stage2 only as the first argument");

    // Round trip of the stage-2 command line, including a folder with spaces.
    for (int s = 0; s < 2; s++) for (int r = 0; r < 2; r++) {
        std::wstring cl = NdmStage2CommandLine(L"C:\\Windows\\Temp\\NDMUninst-ab\\Uninstall.exe",
                                               L"D:\\My Apps\\NDM", 4242, s != 0, r != 0);
        NdmUninstArgs b = Parse(cl);
        CHECK(b.stage2 && b.stage2Dir == L"D:\\My Apps\\NDM" && b.parentPid == 4242 &&
              b.silent == (s != 0) && b.removeData == (r != 0), L"stage2 round trip %s", cl.c_str());
    }
}

static void TestPickPrevious()
{
    const std::wstring nw = L"D:\\Apps\\NDM";
    CHECK(NdmPickPreviousInstallDir({ L"C:\\Program Files\\Devnetra\\NDM" }, nw) == L"C:\\Program Files\\Devnetra\\NDM",
          L"different folder is picked");
    CHECK(NdmPickPreviousInstallDir({ L"C:\\Program Files\\Devnetra\\NDM\\" }, nw) == L"C:\\Program Files\\Devnetra\\NDM",
          L"trailing slash stripped");
    CHECK(NdmPickPreviousInstallDir({ L"d:\\apps\\ndm\\" }, nw).empty(), L"same folder (case/slash) is not picked");
    CHECK(NdmPickPreviousInstallDir({ L"D:\\Apps" }, nw).empty(), L"parent of the new folder is not picked");
    CHECK(NdmPickPreviousInstallDir({ L"D:\\Apps\\NDM\\tools\\NDM" }, nw).empty(), L"folder inside the new one is not picked");
    CHECK(NdmPickPreviousInstallDir({ L"D:\\Apps\\NDM2" }, nw) == L"D:\\Apps\\NDM2", L"sibling with common prefix is picked");
    CHECK(NdmPickPreviousInstallDir({ L"" }, nw).empty(), L"empty");
    CHECK(NdmPickPreviousInstallDir({ L"NDM" }, nw).empty(), L"relative");
    CHECK(NdmPickPreviousInstallDir({ L"\\\\server\\share\\NDM" }, nw).empty(), L"UNC");
    CHECK(NdmPickPreviousInstallDir({ L"C:\\" }, nw).empty(), L"drive root");
    CHECK(NdmPickPreviousInstallDir({ L"C:\\A\\..\\Windows" }, nw).empty(), L"dot-dot");
    CHECK(NdmPickPreviousInstallDir({ L"C:\\A\\.\\NDM" }, nw).empty(), L"dot");
    CHECK(NdmPickPreviousInstallDir({ L"C:/Program Files/NDM" }, nw).empty(), L"forward slashes");
    CHECK(NdmPickPreviousInstallDir({ L"C:\\A:B\\NDM" }, nw).empty(), L"stream colon");
    CHECK(NdmPickPreviousInstallDir({ L"C:\\A\\\\NDM" }, nw).empty(), L"empty component");
    CHECK(NdmPickPreviousInstallDir({ L"", L"d:\\apps\\ndm", L"E:\\NDM" }, nw) == L"E:\\NDM", L"first acceptable candidate");
    CHECK(NdmPathIsInside(L"C:\\a\\b", L"C:\\a") && !NdmPathIsInside(L"C:\\ab", L"C:\\a") &&
          !NdmPathIsInside(L"C:\\a", L"C:\\a") && NdmPathIsInside(L"C:\\a", L"C:\\"), L"NdmPathIsInside");
}

static void TestSidAndProfile()
{
    CHECK(NdmIsPlausibleSid(L"S-1-5-21-1004336348-1177238915-682003330-1001"), L"user SID");
    CHECK(NdmIsPlausibleSid(L"S-1-5-18"), L"SYSTEM");
    CHECK(!NdmIsPlausibleSid(L""), L"empty");
    CHECK(!NdmIsPlausibleSid(L"S-1-5-21-"), L"trailing dash");
    CHECK(!NdmIsPlausibleSid(L"S-1-5--21"), L"double dash");
    CHECK(!NdmIsPlausibleSid(L"S-1-5-21\\..\\x"), L"path characters");
    CHECK(!NdmIsPlausibleSid(L"S-1-5-21-1 "), L"space");
    CHECK(!NdmIsPlausibleSid(L"s-1-5-18"), L"lower case prefix");

    const std::wstring prof = L"C:\\Users\\bob";
    CHECK(NdmExpandProfilePath(L"%USERPROFILE%\\AppData\\Roaming", prof) == L"C:\\Users\\bob\\AppData\\Roaming", L"expand");
    CHECK(NdmExpandProfilePath(L"%userprofile%\\Desktop\\", prof) == L"C:\\Users\\bob\\Desktop", L"case + slash");
    CHECK(NdmExpandProfilePath(L"D:\\Redirected\\Desktop", prof) == L"D:\\Redirected\\Desktop", L"absolute kept");
    CHECK(NdmExpandProfilePath(L"%OneDrive%\\Desktop", prof).empty(), L"other variable refused");
    CHECK(NdmExpandProfilePath(L"%USERPROFILE%\\x", L"").empty(), L"no profile");
    CHECK(NdmExpandProfilePath(L"Desktop", prof).empty(), L"relative refused");
    CHECK(NdmExpandProfilePath(L"%USERPROFILE", prof).empty(), L"unterminated");

    CHECK(NdmIsCachedTorrentName(L"0123456789abcdef0123456789ABCDEF01234567.torrent"), L"hash.torrent");
    CHECK(!NdmIsCachedTorrentName(L"my movie.torrent"), L"user torrent name");
    CHECK(!NdmIsCachedTorrentName(L"0123456789abcdef0123456789abcdef0123456g.torrent"), L"non-hex");
    CHECK(!NdmIsCachedTorrentName(L"0123456789abcdef0123456789abcdef01234567.torrent.mp4"), L"other extension");

    std::wstring self = NdmSelfUserSid();
    std::wstring inter = NdmInteractiveUserSid();
    CHECK(NdmIsPlausibleSid(self), L"own SID %s", self.c_str());
    CHECK(_wcsicmp(inter.c_str(), self.c_str()) == 0, L"interactive user %s == own %s (unelevated test)", inter.c_str(), self.c_str());
    PWSTR p = nullptr;
    std::wstring known;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Profile, 0, nullptr, &p)) && p) known = p;
    if (p) CoTaskMemFree(p);
    std::wstring viaSid = NdmProfileDirOfSid(self);
    CHECK(!viaSid.empty() && _wcsicmp(viaSid.c_str(), known.c_str()) == 0, L"ProfileList %s == %s", viaSid.c_str(), known.c_str());
    CHECK(NdmProfileDirOfSid(L"S-1-5-21-1-2-3-999999").empty(), L"unknown SID has no profile");
}

// ---- file system ------------------------------------------------------------
static bool Touch(const std::wstring& p, const char* text = "x")
{
    HANDLE h = CreateFileW(p.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD w = 0;
    WriteFile(h, text, (DWORD)strlen(text), &w, nullptr);
    CloseHandle(h);
    return true;
}
static bool Exists(const std::wstring& p) { return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES; }
static void MkDirs(const std::wstring& p) { SHCreateDirectoryExW(nullptr, p.c_str(), nullptr); }
static bool Junction(const std::wstring& link, const std::wstring& target)
{
    std::wstring cmd = L"cmd /c mklink /J \"" + link + L"\" \"" + target + L"\" >nul";
    return _wsystem(cmd.c_str()) == 0 && Exists(link);
}
static void Wipe(const std::wstring& p) { bool rb = false; NdmDeleteNoFollow(p, rb, 0, false); }

static void TestUserData(const std::wstring& root)
{
    const std::wstring prof = root + L"\\profile";
    const std::wstring roam = prof + L"\\AppData\\Roaming", local = prof + L"\\AppData\\Local";
    const std::wstring rd = roam + L"\\Devnetra\\NDM", ld = local + L"\\Devnetra\\NDM";
    const std::wstring hash = L"0123456789abcdef0123456789abcdef01234567";
    std::vector<std::wstring> notes;

    // 1. Only NDM's own files: everything goes, including "Devnetra".
    MkDirs(rd + L"\\torrents");
    for (const wchar_t* f : kNdmRoamingDataFiles) Touch(rd + L"\\" + f);
    Touch(rd + L"\\torrents\\" + hash + L".torrent");
    MkDirs(ld + L"\\tools"); MkDirs(ld + L"\\run");
    Touch(ld + L"\\tools\\yt-dlp.exe"); Touch(ld + L"\\tools\\yt-dlp.lastupdate"); Touch(ld + L"\\run\\aria2-ab.conf");
    NdmRemoveNdmDataDir(roam, true, prof, notes);
    NdmRemoveNdmDataDir(local, false, prof, notes);
    CHECK(!Exists(roam + L"\\Devnetra") && !Exists(local + L"\\Devnetra") && notes.empty(),
          L"NDM-only data removed completely (notes %zu)", notes.size());
    CHECK(Exists(roam) && Exists(local), L"AppData folders themselves stay");

    // 2. User files next to NDM's: they stay, so do their folders.
    notes.clear();
    MkDirs(rd + L"\\torrents"); MkDirs(rd + L"\\Downloads");
    Touch(rd + L"\\ndm.ini"); Touch(rd + L"\\queue.xml");
    Touch(rd + L"\\my video.mp4"); Touch(rd + L"\\Downloads\\file.zip");
    Touch(rd + L"\\torrents\\" + hash + L".torrent"); Touch(rd + L"\\torrents\\mine.torrent");
    NdmRemoveNdmDataDir(roam, true, prof, notes);
    CHECK(!Exists(rd + L"\\ndm.ini") && !Exists(rd + L"\\queue.xml") && !Exists(rd + L"\\torrents\\" + hash + L".torrent"),
          L"NDM files removed");
    CHECK(Exists(rd + L"\\my video.mp4") && Exists(rd + L"\\Downloads\\file.zip") && Exists(rd + L"\\torrents\\mine.torrent"),
          L"user files kept");
    CHECK(notes.size() == 1 && notes[0].find(rd) != std::wstring::npos, L"kept folder reported");
    Wipe(roam + L"\\Devnetra");

    // 3. A junction anywhere in the path: nothing is touched behind it.
    notes.clear();
    const std::wstring victim = root + L"\\victim";
    MkDirs(victim + L"\\NDM");
    Touch(victim + L"\\NDM\\ndm.ini"); Touch(victim + L"\\NDM\\queue.xml");
    bool j = Junction(roam + L"\\Devnetra", victim);
    CHECK(j, L"junction created (mklink /J)");
    if (j) {
        NdmRemoveNdmDataDir(roam, true, prof, notes);
        CHECK(Exists(victim + L"\\NDM\\ndm.ini") && Exists(victim + L"\\NDM\\queue.xml"), L"files behind the junction kept");
        CHECK(notes.size() == 1 && notes[0].find(L"link") != std::wstring::npos, L"junction reported");
        RemoveDirectoryW((roam + L"\\Devnetra").c_str());    // removes the junction itself
    }
    notes.clear();
    MkDirs(rd);
    Touch(rd + L"\\ndm.ini");
    bool j2 = Junction(rd + L"\\torrents", victim + L"\\NDM");
    Touch(victim + L"\\NDM\\" + hash + L".torrent");
    if (j2) {
        NdmRemoveNdmDataDir(roam, true, prof, notes);
        CHECK(Exists(victim + L"\\NDM\\" + hash + L".torrent"), L"cache folder junction not followed");
        CHECK(!Exists(rd + L"\\ndm.ini"), L"real files next to it removed");
    }
    Wipe(roam + L"\\Devnetra");

    // 4. Profile guard: another user's folder outside their profile is refused.
    notes.clear();
    const std::wstring outside = root + L"\\elsewhere";
    MkDirs(outside + L"\\Devnetra\\NDM");
    Touch(outside + L"\\Devnetra\\NDM\\ndm.ini");
    NdmRemoveNdmDataDir(outside, true, prof, notes);
    CHECK(Exists(outside + L"\\Devnetra\\NDM\\ndm.ini") && notes.size() == 1, L"outside the profile: refused");
    notes.clear();
    NdmRemoveNdmDataDir(outside, true, L"", notes);
    CHECK(!Exists(outside + L"\\Devnetra"), L"no guard (own profile): removed");

    // 5. Desktop shortcut
    const std::wstring desk = prof + L"\\Desktop";
    MkDirs(desk);
    Touch(desk + L"\\Nee Download Manager.lnk");
    Touch(desk + L"\\other.lnk");
    CHECK(NdmDeleteUserFile(desk, L"Nee Download Manager.lnk", prof) && !Exists(desk + L"\\Nee Download Manager.lnk") &&
          Exists(desk + L"\\other.lnk"), L"shortcut deleted, others kept");
    CHECK(NdmDeleteUserFile(desk, L"Nee Download Manager.lnk", prof), L"missing shortcut = ok");
    MkDirs(desk + L"\\Nee Download Manager.lnk");
    Touch(desk + L"\\Nee Download Manager.lnk\\inner.txt");
    CHECK(!NdmDeleteUserFile(desk, L"Nee Download Manager.lnk", prof) && Exists(desk + L"\\Nee Download Manager.lnk\\inner.txt"),
          L"a folder with that name is not deleted");
    Touch(outside + L"\\Nee Download Manager.lnk");
    CHECK(!NdmDeleteUserFile(outside, L"Nee Download Manager.lnk", prof) && Exists(outside + L"\\Nee Download Manager.lnk"),
          L"shortcut outside the profile refused");
}

// ---- registry ----------------------------------------------------------------
static bool KeyExists(HKEY root, const std::wstring& k)
{
    HKEY h = nullptr;
    if (RegOpenKeyExW(root, k.c_str(), REG_OPTION_OPEN_LINK, KEY_READ, &h) != ERROR_SUCCESS) return false;
    RegCloseKey(h);
    return true;
}
static void MkKey(HKEY root, const std::wstring& k, const wchar_t* val = nullptr)
{
    HKEY h = nullptr;
    if (RegCreateKeyExW(root, k.c_str(), 0, nullptr, 0, KEY_WRITE, nullptr, &h, nullptr) == ERROR_SUCCESS) {
        if (val) RegSetValueExW(h, val, 0, REG_SZ, reinterpret_cast<const BYTE*>(L"v"), 4);
        RegCloseKey(h);
    }
}
// HKCU\<parent>\<name> as a registry symbolic link to HKCU\<target>.
static bool MkLink(const std::wstring& parent, const std::wstring& name, const std::wstring& target)
{
    std::wstring sid = NdmSelfUserSid();
    HKEY hp = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, parent.c_str(), 0, KEY_CREATE_SUB_KEY, &hp) != ERROR_SUCCESS) return false;
    HKEY h = nullptr;
    LSTATUS ls = RegCreateKeyExW(hp, name.c_str(), 0, nullptr, REG_OPTION_CREATE_LINK | REG_OPTION_VOLATILE,
                                 KEY_WRITE | KEY_CREATE_LINK, nullptr, &h, nullptr);
    RegCloseKey(hp);
    if (ls != ERROR_SUCCESS) return false;
    std::wstring t = L"\\REGISTRY\\USER\\" + sid + L"\\" + target;
    ls = RegSetValueExW(h, L"SymbolicLinkValue", 0, REG_LINK, reinterpret_cast<const BYTE*>(t.data()),
                        (DWORD)(t.size() * sizeof(wchar_t)));
    RegCloseKey(h);
    return ls == ERROR_SUCCESS;
}

static void TestRegistry()
{
    const std::wstring rnd = NdmRandomHex(6);
    const std::wstring base = L"Software\\NDM_P4A_Test_" + rnd;     // fake "user hive root"
    const std::wstring target = L"Software\\NDM_P4A_Target_" + rnd;  // must survive
    MkKey(HKEY_CURRENT_USER, target + L"\\Deep", L"keep");

    // a) NDM tree with a link inside, Devnetra otherwise empty -> both gone.
    MkKey(HKEY_CURRENT_USER, base + L"\\Software\\Devnetra\\NDM\\WindowPlacement", L"x");
    MkKey(HKEY_CURRENT_USER, base + L"\\Software\\Devnetra\\NDM", L"BrowserMonitor");
    bool linked = MkLink(base + L"\\Software\\Devnetra\\NDM", L"evil", target);
    CHECK(linked, L"registry link created");
    HKEY root = nullptr;
    RegOpenKeyExW(HKEY_CURRENT_USER, base.c_str(), 0, KEY_READ, &root);
    CHECK(root && NdmRegRemoveUserProductKey(root), L"product key removed");
    CHECK(!KeyExists(HKEY_CURRENT_USER, base + L"\\Software\\Devnetra"), L"empty Devnetra removed");
    CHECK(KeyExists(HKEY_CURRENT_USER, target + L"\\Deep"), L"link target untouched");
    CHECK(KeyExists(HKEY_CURRENT_USER, base + L"\\Software"), L"Software stays");

    // b) Devnetra holds something else -> kept.
    MkKey(HKEY_CURRENT_USER, base + L"\\Software\\Devnetra\\NDM\\a");
    MkKey(HKEY_CURRENT_USER, base + L"\\Software\\Devnetra\\Other");
    CHECK(NdmRegRemoveUserProductKey(root), L"product key removed (b)");
    CHECK(!KeyExists(HKEY_CURRENT_USER, base + L"\\Software\\Devnetra\\NDM") &&
          KeyExists(HKEY_CURRENT_USER, base + L"\\Software\\Devnetra\\Other"), L"sibling product kept");
    NdmRegDeleteTreeNoFollow(HKEY_CURRENT_USER, base + L"\\Software\\Devnetra");

    // c) Devnetra itself a link -> refused, target untouched.
    MkKey(HKEY_CURRENT_USER, target + L"\\NDM", L"keep");
    if (MkLink(base + L"\\Software", L"Devnetra", target)) {
        CHECK(!NdmRegRemoveUserProductKey(root), L"linked Devnetra refused");
        CHECK(KeyExists(HKEY_CURRENT_USER, target + L"\\NDM"), L"target of linked Devnetra untouched");
    }
    // d) nothing there -> fine
    NdmRegDeleteTreeNoFollow(HKEY_CURRENT_USER, base + L"\\Software\\Devnetra");
    CHECK(NdmRegRemoveUserProductKey(root), L"absent key = ok");
    if (root) RegCloseKey(root);

    NdmRegDeleteTreeNoFollow(HKEY_CURRENT_USER, base);
    NdmRegDeleteTreeNoFollow(HKEY_CURRENT_USER, target);
    CHECK(!KeyExists(HKEY_CURRENT_USER, base) && !KeyExists(HKEY_CURRENT_USER, target), L"test keys cleaned up");
}

int wmain(int argc, wchar_t** argv)
{
    if (argc < 2) { wprintf(L"usage: uninsttest <scratch dir>\n"); return 2; }
    std::wstring root = NdmStripSlash(argv[1]) + L"\\uninst_" + NdmRandomHex(4);
    MkDirs(root);
    TestArgs();
    TestPickPrevious();
    TestSidAndProfile();
    TestUserData(root);
    TestRegistry();
    Wipe(root);
    wprintf(L"uninsttest: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
