# Contributing to NDM

Thanks for helping. Bug reports, fixes for sites that stopped working, UI improvements
and translations are all welcome.

## Reporting a bug

Open an issue with:
- NDM version (*Options → About*) and Windows version
- what you did, what you expected, what happened
- for video problems: the page address (if it is public) and the browser

Please do not post private links, cookies or account details.

## Making a change

1. Fork the repository and create a branch.
2. Build with `tools\dev_build.bat build_dev` (see the README for requirements).
3. Keep the style of the file you edit (C++17, Win32, 4-space indent, existing line endings).
4. The extension exists twice: edit `src/browser_ext/chrome/background.js` / `content.js`
   and copy them unchanged to `src/browser_ext/firefox/`.
5. Run the tests in [`tests/README.md`](tests/README.md) and add a test for what you fix.
6. Open a pull request that explains the problem and how you checked the fix.

## Principles

- **Nothing fake.** Every size, quality, bitrate and status shown must come from real data.
  Unknown stays blank; estimates are marked `~`.
- **Never lose user data.** No silent overwrites, no truncating partial downloads that can
  still be resumed.
- **Compact UI.** Only what is needed, short plain text.

By contributing you agree that your contribution is licensed under the MIT License.
