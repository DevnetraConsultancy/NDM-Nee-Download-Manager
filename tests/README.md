# Tests

## Extension (Node.js)

The extension is loaded into a sandbox with stubbed browser APIs. Run each suite against
both copies (`chrome` and `firefox` must stay byte-identical):

```powershell
node tests/extension/test_bg.js            src/browser_ext/chrome/background.js
node tests/extension/test_content.js       src/browser_ext/chrome/content.js
node tests/extension/test_l3.js            src/browser_ext/chrome/background.js
node tests/extension/test_content_media.js src/browser_ext/chrome/content.js
node tests/extension/test_bg_media.js      src/browser_ext/chrome/background.js
```

## Native (C++)

Build NDM first, then the test programs (they link the libraries from the build folder,
`build_dev` by default; set `NDM_BUILD` to use another one):

```powershell
tools\dev_build.bat build_dev
tests\native\mk.bat
tests\native\mkmedia.bat
```

| Program | Covers |
|---|---|
| `uitest.exe` | UI rules: dates, search, clipboard, menus, dialogs, settings round-trips, sidebar maths |
| `qtest.exe <tempdir>` | download list durability (cut-off file, backup), persisted fields, order, scheduler maths |
| `uninsttest.exe <tempdir>` | uninstaller arguments, safe data removal (junctions refused, user files kept) |
| `plsrvtest.exe` | loopback playlist server: only its token path, GET only, closed after use |
| `mediatest.exe` | quality and audio list built from yt-dlp data |

### Engine fault tests

`python tests/native/run_faults.py <tempdir>` starts a local server (`faultsrv.py`) that
misbehaves on purpose — breaks ranges mid-download, changes the file, rotates ETags,
refuses single connections, drops connections — and checks that every download still
finishes with the right SHA-256. It also measures speed limits and priority shares, so
run it on an idle machine. It also runs the network part of `qtest.exe` (refresh address,
auto-retry, live limits, completion actions, scheduler timer).

All tests use temporary folders only; none of them touch an installed NDM or its settings.
