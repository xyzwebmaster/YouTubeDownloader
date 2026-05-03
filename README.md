# YouTube Bulk Downloader

A small native Windows GUI (Win32 / C++17) front-end for
[`yt-dlp`](https://github.com/yt-dlp/yt-dlp) that lets you scan a YouTube
channel and bulk-download its videos and/or Shorts.

![screenshot placeholder](docs/screenshot.png)

## Features

- Paste a YouTube channel URL and scan it for **Videos**, **Shorts**, or **both**.
- All discovered items are listed with checkboxes — **all selected by
  default**, with a scrollbar; you can deselect any you do not want.
- After scanning, choose:
  - **Video** download (with audio) at any resolution from 240p up to the
    highest available (default: highest), and pick a container
    (Auto / mp4 / mkv / webm).
  - **Audio only** with a configurable format (best / mp3 / m4a / opus /
    vorbis / flac / wav / aac) and bitrate (highest / 320 / 256 / 192 /
    160 / 128 / 96 / 64 kbps).
- Progress bar and a live yt-dlp log.
- Single self-contained EXE — no Qt or other DLLs to ship.

## Requirements

The program is a thin GUI on top of `yt-dlp`, so you must install:

1. [`yt-dlp`](https://github.com/yt-dlp/yt-dlp/releases) — must be on your
   `PATH`. Easiest:
   ```
   winget install yt-dlp
   ```
2. [`ffmpeg`](https://www.ffmpeg.org/download.html) — required by yt-dlp
   for muxing video+audio and for audio re-encoding. Easiest:
   ```
   winget install Gyan.FFmpeg
   ```

## Usage

1. Run `YouTubeDownloader.exe`.
2. Paste a channel URL, e.g. `https://www.youtube.com/@SomeChannel`.
3. Choose **All / Videos / Shorts** and press **Scan**.
4. Optionally untick any videos you do not want.
5. Pick **Video** or **Audio only** and the desired quality.
6. Choose an output folder and press **Download selected**.

## Build

Tested with MinGW-w64 (UCRT). C++17.

```
g++ -std=c++17 -O2 -static -mwindows -DUNICODE -D_UNICODE -municode \
    -o YouTubeDownloader.exe main.cpp \
    -lcomctl32 -lcomdlg32 -lshell32 -lole32 -luuid
```

Or with CMake:

```
cmake -B build -G "MinGW Makefiles"
cmake --build build --config Release
```

## License

MIT.
