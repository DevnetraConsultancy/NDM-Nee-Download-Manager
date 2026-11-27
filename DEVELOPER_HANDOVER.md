# NDM — Developer Handover

**Nee Download Manager (NDM)** — a native Windows download manager by **Devnetra Consultancy**, MIT-licensed.
This document describes the whole project in enough detail to **rebuild the same software from scratch**:
the toolchain, the build steps, every source module, the data formats, the IPC protocols and the release
process. It states only what the code actually does; nothing here is aspirational.

> Scope check: this is the handover for the codebase as it stands at **version 1.0.6**. If a detail here and
> the code ever disagree, the code wins — this file is a map, not the territory.

---

## 1. At a glance

| | |
|---|---|
| Product | Nee Download Manager (NDM) |
| Version | 1.0.6 (`src/common/ndm_common.h` → `NDM_VERSION_STR`) |
| Language | C++17, Win32 API (no MFC, no Qt, no .NET) |
| Target | Windows 10 / 11, **x64 only** |
| Toolchain | MSVC v143 (Visual Studio 2022), CMake ≥ 3.20, Ninja |
| CRT | Static (`/MT`) — no `vcruntime140.dll` beside the exe |
| GUI | Raw Win32 + GDI/GDI+ (owner-drawn "dark gold" theme) |
| License | MIT, © 2026 Devnetra Consultancy |
| Repo | https://github.com/DevnetraConsultancy/NDM-Nee-Download-Manager |
| Installer output | `dist\NDM-1.0.6-x64-Setup.exe` (single, offline, ~121 MB) |

**External engines bundled** (not compiled; fetched as pinned binaries into `third_party\` and shipped in
`<install>\tools\`):

| Tool | Purpose | Pinned version |
|---|---|---|
| `yt-dlp.exe` | video/audio extraction | 2026.11.23 |
| `ffmpeg.exe` / `ffprobe.exe` + av\* DLLs | mux/convert audio & video | BtbN build `autobuild-2026-11-23-12-43` (FFmpeg N-129241, win64 **LGPL shared**) |
| `deno.exe` | JS runtime yt-dlp needs for some sites (e.g. YouTube) | v2.9.8 |
| `aria2c.exe` | BitTorrent / magnet engine | release-1.37.0 |

Exact download URLs and SHA-256 pins live in `tools/fetch_tools.ps1` and `tools/fetch_aria2.ps1` (see §19).

---

## 2. Prerequisites

Install on a Windows x64 machine:

1. **Visual Studio 2022** (or Build Tools 2022) with:
   - *Desktop development with C++* → **MSVC v143 x64/x86 build tools**
   - **Windows 10/11 SDK**
   - *(optional)* **MSVC v143 Spectre-mitigated libs** — if present, the build adds `/Qspectre` automatically; if not, it builds without it.
2. **CMake ≥ 3.20** (on `PATH`, or point `NDM_CMAKE` at `cmake.exe`).
3. **Ninja** (on `PATH`, or `NDM_NINJA`).
4. **PowerShell 5.1** (ships with Windows) — for the fetch and SFX-packing scripts.
5. **Python 3** and **Node.js** — only for the test suites (not for the build itself).

No source dependencies are vendored or pulled from the internet during the C++ build. The only network
access is the one-time `fetch_*.ps1` run that downloads the four external engines.

---

## 3. Get the external engines (one-time, needs internet)

These binaries are **not** in git (`.gitignore` excludes `third_party/*.exe` and `third_party/*.dll`). Fetch
them before a full installer build:

```powershell
powershell -ExecutionPolicy Bypass -File tools\fetch_tools.ps1
powershell -ExecutionPolicy Bypass -File tools\fetch_aria2.ps1
```

Both scripts are **pinned and fail-closed**: they download only the exact GitHub release assets listed in the
script, verify the SHA-256 of the archive **and** of every extracted file, and write nothing to
`third_party\` unless every hash matches. Files already present are re-verified against the same pins.
`-Force` re-fetches; `-Update` (maintainer only) pulls the latest releases and prints a new pin block to paste
back into the script.

If you restored from the `NDM-source-1.0.6.zip` bundle, `third_party\` is already populated and you can skip
this step.

---

## 4. Build

### 4a. Developer build (binaries only, fast)

```powershell
tools\dev_build.bat build_dev
```

Configures once (Release, Ninja, MSVC x64) into `build_dev\`, then builds all targets. Re-running only
rebuilds what changed. Outputs land in `build_dev\bin\`. You can pass explicit Ninja targets:
`tools\dev_build.bat build_dev NDMan`.

### 4b. Full offline installer

```powershell
build_sfx.bat
```

Steps performed (`build_sfx.bat`):
1. Locate `vcvars64.bat` (via `vswhere`), CMake and Ninja (overridable with `NDM_VCVARS`, `NDM_CMAKE`, `NDM_NINJA`).
2. Wipe `build\`, configure (Release/Ninja/MSVC x64), build **all** targets (`-j4`).
3. Run `tools\make_sfx_payload.ps1` to append an **NDM-SFX v3** payload to the installer stub
   (`ndmsetup643build1.exe`). The payload bundles: the built binaries from `build\bin\`, the browser
   extension from `src\browser_ext\`, and everything in `third_party\` — each entry compressed
   (LZMS / XPRESS_HUFF via `cabinet.dll`) and SHA-256-verified at extract time.
4. *(Optional)* Authenticode-sign the result if `NDM_SIGN_CERT` (a `.pfx`) is set; `NDM_SIGN_PASSWORD`
   and `NDM_SIGN_TIMESTAMP` are optional. The EXE is signed **after** the payload is appended (the stub
   finds its payload in front of the certificate table).
5. Print the final path, size and SHA-256.

Output: `dist\NDM-1.0.6-x64-Setup.exe`.

### Build-time hardening (all targets, from `CMakeLists.txt`)

Static CRT; `/guard:cf` (Control Flow Guard); `/CETCOMPAT`; `/DYNAMICBASE /HIGHENTROPYVA` (ASLR);
`/NXCOMPAT` (DEP); `/GS /sdl`; `/DEPENDENTLOADFLAG:0x800` (static imports resolved from System32 only);
`/Qspectre` when the Spectre libs are installed. The installer and uninstaller additionally **delay-load**
every non-KnownDLL, loading it only after `SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32)`.

---

## 5. Repository layout

```
NDM/
├─ CMakeLists.txt            Single CMake project: 5 libs + 5 binaries
├─ build.bat                 Thin dev wrapper
├─ build_sfx.bat            Full installer build (see §4b)
├─ CHANGELOG.md  README.md  SECURITY.md  CONTRIBUTING.md  LICENSE  THIRD_PARTY_NOTICES.txt
│
├─ src/
│  ├─ common/     ndm_common.{h,cpp}     Shared types, enums, constants, helpers (version, theme colours,
│  │                                     NdmDownload, NdmConfig, DPAPI, Mark-of-the-Web, URL checks)
│  ├─ network/    ndm_http.{h,cpp}       WinHTTP segmented download engine (NdmHttpEngine)
│  ├─ core/
│  │   ├─ ndm_queue.{h,cpp}             Download list, persistence, scheduling, bandwidth allocator
│  │   ├─ ndm_config.{h,cpp}            ndm.ini + registry settings (NdmConfigMgr)
│  │   └─ NDMan.cpp                     WinMain: entry point, single-instance, startup/shutdown
│  ├─ scheduler/  ndm_scheduler.{h,cpp} Pure schedule maths (start/stop/next-event)
│  ├─ media/      ndm_media.{h,cpp}     yt-dlp/ffmpeg runner + loopback playlist server
│  ├─ torrent/    ndm_torrent*.{h,cpp}  aria2c engine + torrent add dialog
│  ├─ msghost/    ndm_msghost.{h,cpp}   Native-messaging ↔ named-pipe bridge (embedded + standalone)
│  │              NDMMsgHost_{chrome,firefox}.json   Native host manifests
│  ├─ ui/         ndm_*.{h,cpp}         All Win32 windows, dialogs, theme, logic (see §8)
│  ├─ shellext/   NDMShellExt.cpp/.def  Explorer right-click COM extension (x64 DLL)
│  ├─ installer/  ndmsetup.cpp + ndm_*.h  SFX installer (see §15)
│  ├─ uninstaller/ Uninstall.cpp + ndm_uninst_logic.h  Uninstaller (see §16)
│  ├─ browser_ext/
│  │   ├─ chrome/   MV3 extension (service worker) — Chrome / Edge
│  │   ├─ firefox/  MV2 extension — background.js and content.js are byte-identical to chrome/
│  │   ├─ images/   shared icons
│  │   ├─ pack_extensions.bat  register_msghost.bat
│  └─ grabber/    (reserved, currently empty)
│
├─ resources/   NDMan.rc  NDMan.manifest  icons/ (ndm.ico, ndm_logo.svg, png/*)
├─ tools/       dev_build.bat  fetch_tools.ps1  fetch_aria2.ps1  make_icons.ps1  make_sfx_payload.ps1
├─ tests/       extension/ (Node suites)  native/ (C++ test programs + Python fault server)
├─ docs/        images/ (README art)  icons/ (Lucide SVGs for the README)
├─ keys/        Browser-extension signing key (NOT in git — see §21)
├─ third_party/ Fetched engine binaries (NOT in git — see §3)
└─ .github/     workflows/ci.yml + issue / PR templates
```

---

## 6. Architecture — process model

NDM runs as **one GUI process** (`NDMan.exe`) that owns the download list, the UI and all three engines.
The browser talks to it through a small relay:

```
Browser extension (Chrome/Edge MV3, Firefox MV2)
      │  native messaging (stdio: 4-byte LE length + UTF-8 JSON)
      ▼
NDMMsgHost.exe        ← one short-lived relay per browser connection
      │  named pipe  \\.\pipe\NDMMsgHost-<user SID>-<session id>
      ▼                 (DACL: SYSTEM + owner; both ends verify each other)
NDMan.exe  ────────────────────────────────────────────────┐
  ├─ NdmHttpEngine   (WinHTTP, segmented)    → plain files   │
  ├─ NdmMediaRun     (yt-dlp + ffmpeg + deno) → video/audio  │  all run as worker
  ├─ NdmTorrentRun   (aria2c, local JSON-RPC) → torrents     │  threads; the queue
  └─ NdmQueue        (list, persistence, bandwidth allocator)┘  owns them
```

Key invariants:
- `NDMan.exe` always runs **unelevated** (medium integrity). Only the installer/uninstaller are elevated.
- No heap pointer ever travels in a window message. Browser requests are queued and the window receives only
  an opaque **token** in `WM_NDM_BROWSER_MSG` which it redeems with `NdmTakeBrowserRequest()`.
- Single instance: a mutex `NDM_App_Mutex_x9kZ2mQ1` guards it; a second launch forwards its URL (or a "show"
  request) via `WM_COPYDATA` and exits (`NDMan.cpp`).

### Command line (`NDMan.exe`)
- `-tray` / `/tray` / `--tray` / `-minimized` — start hidden in the notification area.
- `-firstrun` — force the first-run questions (the installer passes this).
- `-url <address>` or a bare `http(s)://…`, `magnet:?…`, or `.torrent` path — open "Download File Info".

---

## 7. Build units (CMake targets)

| Target | Type | Output | Links |
|---|---|---|---|
| `ndm_common` | static lib | — | (base) |
| `ndm_network` | static lib | — | ndm_common, **winhttp** |
| `ndm_core` | static lib | — | ndm_common, ndm_network, advapi32, shell32 (contains queue, config, scheduler, media, torrent) |
| `ndm_msghost_lib` | static lib | — | ndm_common |
| **NDMan** | GUI exe | `NDMan.exe` | the four libs + comctl32, shell32, ole32, gdiplus, uxtheme, dwmapi, powrprof, winmm, … |
| **ndmsetup** | GUI exe | `ndmsetup643build1.exe` | ndm_common + cabinet, bcrypt, wtsapi32 (installer stub) |
| **Uninstall** | GUI exe | `Uninstall.exe` | advapi32, shell32, bcrypt, wtsapi32 |
| **NDMShellExt** | DLL | `NDMShellExt.dll` | shell32, ole32 (Explorer COM handler) |
| **NDMMsgHost** | console exe | `NDMMsgHost.exe` | ndm_common (built with `NDM_MSGHOST_STANDALONE=1`) |

The odd installer name `ndmsetup643build1.exe` is the stub; `build_sfx.bat` renames the payload-appended
result to `NDM-<version>-x64-Setup.exe`.

---

## 8. UI layer (`src/ui/`)

Pure Win32, owner-drawn. The theme is the "Premium Dark Gold" palette defined as `NDM_CLR_*` macros in
`ndm_common.h`.

| File | Responsibility |
|---|---|
| `ndm_theme.{h,cpp}` | GDI/GDI+ primitives, fonts, dark title bar, owner-draw buttons/toggles, DPI scaling, modeless-dialog registry, the NDM logo/glyphs, category helpers |
| `ndm_mainwnd.{h,cpp}` | Main window: toolbar, sidebar (categories + status), the download **ListView** (owner-drawn rows), search, clipboard watcher, drag-and-drop **in** (links/.torrent) and the tray icon |
| `ndm_adddlg.{h,cpp}` | "Download File Info" / Add-URL dialog |
| `ndm_progdlg.{h,cpp}` | Per-download window: Download status / Speed Limiter / Options-on-completion tabs, plus the **drag-out file tile** (1.0.6) |
| `ndm_filedrag.{h,cpp}` | Drag a finished file **out** of NDM (OLE data object over the file's shell ID list). Plain drag = move (NDM follows the file via shell change notifications), Shift = copy |
| `ndm_optdlg.{h,cpp}` | Options dialog; applies settings and the "start with Windows" Run key |
| `ndm_scheddlg.{h,cpp}` | Scheduler dialog |
| `ndm_firstrun.{h,cpp}` | IDM-style first-run questions |
| `ndm_dialogs.{h,cpp}` | Small themed dialogs: question-with-buttons (`AskEx`), one-line input, duplicate / refresh prompts; worker-wait on exit |
| `ndm_power.{h,cpp}` | Sleep / Hibernate / Shut down after completion, with a cancellable countdown |
| `ndm_uilogic.{h,cpp}` | Pure UI logic (date formatting, column widths, category classification, action names) — unit-tested |
| `ndm_resource.h` | Control/command IDs |

---

## 9. Data model & storage

### `NdmDownload` (the core record, `ndm_common.h`)
Holds: `id`, `url`, `filename`, `savePath`, sizes, `numConnections`, live `speed`, `status`
(`NDM_STATUS_*`), `kind` (`NDM_KIND_HTTP|MEDIA|TORRENT`), `referrer`, `cookies`/`cookieJar` (and their DPAPI
blobs `cookiesEnc`/`cookieJarEnc`), media fields (`pageUrl`, `mediaFormat`, `mediaTemps`), torrent fields
(`torrentFiles`, `torrentHash`), per-download `speedLimit` and `priority` (`NDM_PRIO_LOW|NORMAL|HIGH`,
bandwidth weights **1 / 2 / 4**), `errorKind` (`TRANSIENT`→auto-retried, `FATAL`→needs the user),
`addedTime`/`completedTime` (UTC FILETIME; 0 = unknown = shown blank), `queueOrder`, `scheduled`,
`completionAction`, and live engine telemetry (`resumable`, `activeConns`, `fileMap[32]` permille-per-32nd).

### Where things live
- **Settings:** `%APPDATA%\Devnetra\NDM\ndm.ini` — `[General]` (save path, max connections, max simultaneous
  downloads, browser monitor, start-with-Windows, close-to-tray, global speed limit, retry count/max delay,
  schedule) and `[UI]` (column widths, sort column/order, tray-hint shown). Written with
  `WritePrivateProfileStringW`.
- **Download list:** `%APPDATA%\Devnetra\NDM\queue.xml` (`<NDMQueue version="2">` with one `<Download …/>`
  per item). Saved atomically via `ReplaceFileW` with a `queue.xml.bak` backup; a truncated file is recovered
  from `.bak`, a corrupt file is kept aside as `queue.xml.corrupt`.
- **Partial HTTP downloads:** `<file>.ndmpart` (the data) + `<file>.ndmstate` (ranges already on disk, written
  with a `ranges=` line and `FlushFileBuffers` before the state is saved — this is what makes resume survive a
  power cut).
- **Registry:** `HKCU\Software\Devnetra\NDM` (app state, browser-monitor mirror); the "start with Windows"
  value under `HKCU\…\Run` (`"…\NDMan.exe" -tray`); `HKLM\…\Uninstall\Nee Download Manager` (written by the
  installer).
- **Download folder:** the user's Downloads by default, with IDM-style category subfolders (Compressed,
  Documents, Music, Programs, Video, Pictures) created on start.

---

## 10. HTTP download engine (`src/network/ndm_http.cpp` — `NdmHttpEngine`)

WinHTTP, multi-connection with **dynamic segmentation**: up to 32 connections per file (8 by default); idle
connections split the slowest remaining part. Resume model:
- Probes `Accept-Ranges`; falls back to a single connection (`oneConn`) when the server will not range.
- `SameRemoteFile` guards resume: compares Last-Modified / ETag (tolerant of CDN ETag rotation) before
  continuing a `.ndmpart`, so a server-side file change is detected instead of stitching two versions.
- A range error on one connection never restarts from zero; one refused connection never fails the whole
  download — automatic retry with back-off (`retryCount`, `retryMaxDelay` from config).
- "Refresh download address": re-probe a new URL, confirm it is the same file, rebind the `.ndmstate`, continue.
- Auto-resume when the network returns or the PC wakes (`NotifyAddrChange` watcher).
- The **bandwidth allocator** (`RecomputeFlows` in `ndm_http.cpp`, policy in `ndm_http.h`) uses priority
  weights High 4 / Normal 2 / Low 1. **With** a global limit it does weighted max-min water-filling of the
  rate across flows (pinned media flows reserve what they started with; no flow is starved below a small
  floor). **Without** a global limit, Low flows yield to actively-transferring High/Normal flows: the Low
  flows together get about a quarter of the High/Normal throughput (`max(32 KB/s, hi/4)`), split among them.
  Per-download limits are always honoured on top.
- Every finished file gets **Mark-of-the-Web** (`NdmMarkOfTheWeb`, via `IAttachmentExecute`, falling back to a
  `Zone.Identifier` stream) so SmartScreen checks it.

---

## 11. Media pipeline (`src/media/ndm_media.cpp`)

- Runs the bundled `yt-dlp.exe` (with `ffmpeg.exe` for muxing and `deno.exe` as the JS runtime some sites
  need). The browser's exact cookies are passed as a Netscape `cookies.txt` jar (`cookieJar`), never shared
  cross-site.
- The quality list and the real audio-track list (codec + bitrate, e.g. *Best Audio (Opus 160 kbps)*) are
  built from yt-dlp's `-J` JSON — see `NdmMsgHost::MediaReplyFromJson` and the `mediatest.exe` suite. MP3
  conversions are marked *(converted)* and never exceed the source bitrate.
- Joined HLS/DASH streams are served to yt-dlp through a **loopback PlaylistServer** (a localhost HTTP server
  bound to a random token path, GET-only, closed after use — no `file://` URLs). Tested by `plsrvtest.exe`.
- Only the intermediate files yt-dlp itself reports (`mediaTemps`) are ever deleted as temp files.

---

## 12. Torrents (`src/torrent/`)

- `ndm_torrent.cpp` drives the bundled `aria2c.exe` over a local JSON-RPC port. Magnet links and `.torrent`
  files, with per-file selection (`torrentFiles` = aria2 `--select-file` indexes).
- `ndm_torrent_dlg.cpp` is the file-selection dialog (uses the UI theme helpers, hence it is compiled into
  `NDMan`).
- On shutdown the engine does `saveSession` + a graceful aria2 stop so control files are flushed and resume
  works even if the original link is gone.

---

## 13. Browser extension + native messaging

### The relay (`src/msghost/ndm_msghost.cpp`)
Two build modes from one source:
- **Embedded** (in `NDMan.exe`): `StartListening()` runs a multi-client **named-pipe server** at
  `\\.\pipe\NDMMsgHost-<user SID>-<session id>`, DACL = SYSTEM + owner. `addDownload` / `addDownloads` become
  `NdmBrowserRequest`s.
- **Standalone** (`NDMMsgHost.exe`, built with `NDM_MSGHOST_STANDALONE=1`): speaks the Chrome/Firefox native
  messaging protocol on stdin/stdout and relays to `NDMan.exe` over the pipe, launching `NDMan.exe -tray` if
  needed.

**Wire format on both legs:** 4-byte little-endian length + that many bytes of UTF-8 JSON (≤ 16 MiB), one
reply per request, in order. Native host name: `com.devnetra.ndm`. Both ends verify identity: the server
checks the client runs as the same user; the relay checks the server is `<its own folder>\NDMan.exe` of the
same user.

### The extension (`src/browser_ext/`)
- `chrome/` is **MV3** (service-worker `background.js`); `firefox/` is **MV2**. `background.js` and
  `content.js` are **byte-identical** between the two copies (CI enforces this with a hash check).
- Manifest facts (`chrome/manifest.json`): permissions `downloads, contextMenus, cookies, storage,
  notifications, nativeMessaging, webRequest, scripting`; `host_permissions: <all_urls>`; fixed `key` (pins
  the extension ID — see §21); `minimum_chrome_version 116`.
- Behaviour: a corner download button on videos/photos (500 ms hover prefetch, click takes priority over
  hover), *Download with NDM* / *Download all links* context menus, capture of browser downloads **only when
  NDM is running** (a `capture` flag mirrored from `HKCU` BrowserMonitor), POST-result and private-window
  downloads are left alone, sniffed media is remembered across service-worker restarts (`storage.session`).

---

## 14. Installer (`src/installer/ndmsetup.cpp`)

- **GUI only — no command-line switches.** (The former `/extract-only` and `/preview` test switches were
  removed: an elevated binary must not write into a folder named on its command line.)
- Manifest requests `requireAdministrator` (needed for `Program Files` and `HKLM`); PerMonitorV2 DPI.
- Installs offline to `C:\Program Files\Devnetra\NDM`, lays down the binaries + `tools\` (the four engines) +
  the browser extensions, writes the `HKLM\…\Uninstall\Nee Download Manager` entry (with
  `QuietUninstallString` and the interactive user's SID so the uninstaller can clean per-user data), creates
  shortcuts, optionally starts `NDMan.exe -firstrun`.
- Cleans up a previous x64 install first.
- **Payload format NDM-SFX v3** (`ndm_sfx.h` + `sfx_extract.h`): entries compressed with the Windows
  Compression API (LZMS / XPRESS_HUFF via `cabinet.dll`), each SHA-256-verified while streaming out. The stub
  finds its payload in front of the PE certificate table, so signing after packing works.
- Protected ACL on the install folder; file operations never follow junctions/reparse points
  (`ndm_fsutil.h`).

---

## 15. Uninstaller (`src/uninstaller/`)

- Flags (`ndm_uninst_logic.h`, unit-tested): `Uninstall.exe [/S | /quiet] [/removedata]`.
  `/S` (or `/quiet`) = no message boxes; `/removedata` = also remove per-user settings and the download list.
  Internal: `/stage2 "<install dir>" <pid> [/S] [/removedata]` — the exe copies itself to a temp folder
  (random `bcrypt` name) and runs stage-2 from there so it can delete its own folder.
- Safe removal: junction/reparse targets are refused, the user's **downloaded files are never touched**, only
  NDM's own install content + registry keys + (with `/removedata`) `%APPDATA%\Devnetra\NDM` and
  `HKCU\Software\Devnetra`.

---

## 16. Shell extension (`src/shellext/NDMShellExt.cpp`)

x64 COM in-proc handler loaded by Explorer for the right-click *Download with NDM* entries. The installer
writes its COM registry keys directly; `DllRegisterServer`/`DllUnregisterServer` are exported
(`NDMShellExt.def`) for `regsvr32` during development.

---

## 17. Security model (summary)

- `NDMan.exe` never elevates. The browser pipe is per-user, DACL-restricted, mutually authenticated.
- Cookies handed over by the browser are stored **DPAPI-encrypted** (`NdmProtectString`, user scope) and only
  sent to the site they belong to; cross-site redirects never carry them. Referer follows
  strict-origin-when-cross-origin (`NdmReferrerFor`).
- File names are sanitised; deceptive bidi/zero-width characters (`NdmIsDeceptiveChar`) are mapped to `_`.
- DLLs load from System32 + the app folder only; static CRT; CFG/CET/ASLR/DEP; delay-load in the elevated
  installer/uninstaller.
- Downloads get Mark-of-the-Web. See `SECURITY.md` for the reporting policy.

---

## 18. Tests & CI

### Extension (Node) — `tests/extension/`
Five suites run against **both** extension copies; a hash check enforces that the chrome/firefox
`background.js` and `content.js` stay identical:
`test_bg.js`, `test_content.js`, `test_l3.js` (cookie SameSite), `test_content_media.js`, `test_bg_media.js`.

### Native (C++) — `tests/native/` (build with `mk.bat` / `mkmedia.bat` after a dev build)
| Program | Covers |
|---|---|
| `uitest.exe` | UI rules: dates, search, clipboard, menus, dialogs, settings round-trips, sidebar maths |
| `qtest.exe <tmp>` | list durability (cut-off file → `.bak`, corrupt → `.corrupt`), persisted fields, order, **relocate-after-drag-out**, scheduler maths; plus the network part (refresh address, auto-retry, live limits, completion actions) |
| `uninsttest.exe <tmp>` | uninstaller arguments, safe data removal (junctions refused, user files kept) |
| `plsrvtest.exe` | loopback playlist server (token path only, GET only, closed after use) |
| `mediatest.exe` | quality + audio list from yt-dlp JSON |
| `run_faults.py <tmp>` | `faultsrv.py` breaks ranges / changes files / rotates ETags / refuses connections; checks every download still finishes with the right SHA-256, and measures speed-limit & priority shares (run on an idle machine) |

All tests use temporary folders only — they never touch an installed NDM or its settings.

### CI — `.github/workflows/ci.yml` (windows-2022)
Runs the Node extension suites (both copies + the identical-copies check), installs Ninja (choco), builds
Release via `tools\dev_build.bat`, then builds and runs `uitest`, `plsrvtest`, `mediatest`, `uninsttest`,
`qtest`. `run_faults.py` is **not** run in CI (its timing checks can be flaky on shared runners).

---

## 19. Third-party pins (exact versions & hashes)

From `tools/fetch_tools.ps1` and `tools/fetch_aria2.ps1` (recorded 2026-11-24). All sources are GitHub
releases; the scripts refuse any non-GitHub URL.

| Tool | Tag / asset | SHA-256 (key file) |
|---|---|---|
| yt-dlp | `2026.11.23` → `yt-dlp.exe` | `622ed48023cac1daa52a3bf3a6ac38f03514de214b5f0fa069389fd0f6d17e5c` |
| ffmpeg | `autobuild-2026-11-23-12-43` → win64 LGPL shared zip | zip `824507d0254cbc1b52ab7ee4ef402c62e61807ed21d99bea7cdeaeb01eabc3c2` (ffmpeg.exe `b13119e4…`) |
| deno | `v2.9.8` → `deno.exe` | `924397800533d3eb0ed0c889b20ad70e18b970d93acd8135bc043f425e528266` |
| aria2 | `release-1.37.0` → `aria2c.exe` | `be2099c214f63a3cb4954b09a0becd6e2e34660b886d4c898d260febfe9d70c2` |

ffmpeg ships **LGPL shared** (DLLs kept separate from the exe) to respect the licence; the DLL hashes are in
the script. `THIRD_PARTY_NOTICES.txt` records licences (yt-dlp Unlicense, FFmpeg LGPL v3, aria2 GPL-2.0, Deno
MIT); README icons are Lucide (ISC).

---

## 20. Signing / identity keys (`keys/` — not in git)

`keys/` holds the **browser-extension signing material**, kept out of git by `.gitignore`:
- `ndm_ext_private.pem` — the RSA private key.
- `ndm_ext_public.b64` / `.der` — the public key; the b64 is the `"key"` field pinned in both
  `manifest.json`s (it fixes the extension ID across rebuilds).
- `ndm_ext_id.txt` — the resulting extension ID.

**Keep this folder safe and out of any public release.** Without it, a rebuild produces a browser extension
with a *different* ID. It is included in the `NDM-source-1.0.6.zip` bundle but not on GitHub. The Authenticode
code-signing certificate for the installer (`NDM_SIGN_CERT`) is separate and not part of the repo.

---

## 21. Versioning & release

Version strings to bump together for a release (all currently `1.0.6`):
`src/common/ndm_common.h` (`NDM_VERSION_STR`/`_W`), `CMakeLists.txt` (`project(... VERSION)`),
`build_sfx.bat` (output name), `resources/NDMan.rc`, `src/installer/ndmsetup.{rc,manifest,cpp}`,
`src/uninstaller/Uninstall.rc`, `src/msghost/ndm_msghost.cpp` (status reply), and both
`src/browser_ext/*/manifest.json`. Then update `CHANGELOG.md`.

Release steps used for 1.0.6: run the tests → `build_sfx.bat` → `gh release create vX.Y.Z dist\NDM-…-Setup.exe`
with notes and the SHA-256 → the GitHub Actions CI must pass on the pushed commit.

---

## 22. Known limitations (today)

- Windows x64 only (10 / 11).
- Installer not code-signed by default → SmartScreen warns on first run until a certificate is supplied.
- The browser extension is loaded unpacked; on Firefox it is a temporary add-on until signed by Mozilla.
- Installer is ~121 MB because it bundles FFmpeg, yt-dlp, Deno and aria2.
- No whole-site grabber (`src/grabber/` is a reserved, empty placeholder).
- Video sites change; yt-dlp is kept current but a site can break briefly.

---

## 23. Rebuild-from-scratch checklist

1. Windows x64 + VS 2022 (C++ x64, Windows SDK), CMake ≥ 3.20, Ninja. (Python 3 + Node.js for tests.)
2. Get the source: this repo (or the `NDM-source-1.0.6.zip` bundle, which also carries `keys/` and
   `third_party/`).
3. If `third_party/` is empty: `tools\fetch_tools.ps1` + `tools\fetch_aria2.ps1` (needs internet; pins verified).
4. Dev build: `tools\dev_build.bat build_dev` → run the test suites (§18).
5. Full installer: `build_sfx.bat` → `dist\NDM-1.0.6-x64-Setup.exe`.
6. *(optional)* Set `NDM_SIGN_CERT` to Authenticode-sign; keep `keys/` to preserve the extension ID.

---

*Nee Download Manager · © 2026 Devnetra Consultancy · MIT License.*
