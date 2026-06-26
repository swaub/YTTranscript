# YTTranscript installer

Builds `YTTranscript-Setup-x.y.z.exe` with [Inno Setup 6](https://jrsoftware.org/isinfo.php).

The installer is intentionally tiny: it ships only `YTTranscript.exe`. All heavy
components (yt-dlp, FFmpeg, whisper.cpp, llama.cpp, and the models) are
downloaded by the app on first run.

## Prerequisites

1. Build the app first so `dist\YTTranscript.exe` exists:
   ```sh
   bash build.sh
   ```
2. Install [Inno Setup 6](https://jrsoftware.org/isdl.php).

## Build the installer

From the project root (Windows):

```bat
"C:\Program Files (x86)\Inno Setup 6\ISCC.exe" installer\YTTranscript.iss
```

The signed-and-sealed installer lands in `installer\Output\`.

## What the installer does

- Installs per-user (no admin / UAC), defaulting to a writable per-user folder.
- Validates that the chosen install folder is **writable** (the app stores its
  downloaded components next to itself) and warns if you pick somewhere like
  `Program Files`.
- Creates Start Menu and (optional) Desktop shortcuts.
- Offers to launch the app when finished.
- On uninstall, asks whether to also delete the downloaded components/data
  (defaults to keeping them, to avoid a ~2 GB re-download on reinstall).

To bump the version, edit `#define AppVersion` at the top of
`YTTranscript.iss` (and the `VERSIONINFO` in `res/app.rc`).
