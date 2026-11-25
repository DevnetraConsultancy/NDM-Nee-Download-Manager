# Changelog

## 1.0.5 — 2026-11-25

First public release.

### Downloads
- Multi-connection engine (up to 32 connections, dynamic segmentation), pause / resume, resume after restart.
- Progress survives power cuts: data is flushed before progress is saved; the download list keeps a backup.
- A range error on one connection no longer restarts the download from zero.
- Server-side file changes are detected (size, ETag, Last-Modified), including CDNs that rotate ETags.
- Refresh download address for expired links; NDM also offers it when the browser downloads the same file again.
- One refused connection no longer fails the whole download; automatic retry with back-off; auto-resume after network loss and sleep.

### Video, audio and photos
- Icon-only download button in the corner of videos, audio players and photos.
- Best quality first, then every resolution; video and audio merged into one file.
- Audio list with real codec and bitrate (*Best Audio (Opus 160 kbps)*), original tracks without re-encoding, MP3 conversions marked *(converted)* and never above the source bitrate.
- X: the clicked video of multi-video posts; timeline videos always with audio.
- Full-resolution photos on Instagram, X, Pinterest and Reddit.

### Control
- Per-download speed limit and priority, global limit from the tray.
- Scheduler (start / stop times, days, one-off dates, max simultaneous downloads).
- Sleep, Hibernate or Shut down when downloads finish, with a 30-second cancel window.
- Date column, search, queue order, rename, delete with file (to the Recycle Bin), clipboard watching, drag and drop.

### Browser extension
- Chrome / Edge (MV3) and Firefox (MV2).
- Leaves downloads alone when NDM is closed, for form (POST) results and in private windows.
- Clicks are never queued behind hover previews; remembers sniffed media across extension restarts.

### Installer
- Offline single-file installer (~120 MB), clean upgrade, silent uninstall (`/S`, `/removedata`).
