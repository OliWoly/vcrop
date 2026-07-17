# vcrop

A small native tool for visually cropping and trimming videos. Open a
video, drag a rectangle over it and/or mark trim start/end points, and
either process directly or copy a ready-to-paste `ffmpeg` command. Built
with C++17, SDL2, Dear ImGui, and the FFmpeg libraries. Targets macOS
and Linux.

## Usage

```sh
vcrop video.mp4
```

- Drag a rectangle over the video to choose the crop region (any
  direction); **Reset** removes it.
- The panel shows the hovered pixel and the selection as `x / y / w / h` in
  video pixels. Dimensions are rounded to even values, as most encoders
  require.
- **Set start** / **Set end** (or the `i` / `o` keys) mark a trim range at
  the current playback position. Leave either unset to trim from the
  beginning or to the end; **Clear** removes the range.
- The action button reflects what will happen — **Crop**, **Trim**, or
  **Crop + Trim** — and starts `ffmpeg` detached in the background, then
  closes the app. The output appears next to the input with a matching
  suffix (`video_cropped.mp4`, `video_trimmed.mp4`,
  `video_cropped_trimmed.mp4`).
- **Copy command** puts the equivalent shell command on the clipboard,
  prints it verbatim to stdout as a fallback, and closes the app.
- Space toggles play/pause, Esc quits, the slider seeks. Playback is
  video-only; audio is passed through untouched (`-c:a copy`) when
  processing.

Requires the `ffmpeg` binary on `PATH` at crop time.

Note for Linux: on X11/Wayland the clipboard may not survive the app closing
unless a clipboard manager is running — the command is also printed to the
terminal for that reason.

## Building

Dependencies: CMake ≥ 3.24, a C++17 compiler, pkg-config, SDL2, and the
FFmpeg development libraries. Dear ImGui is fetched automatically by CMake.

macOS:

```sh
brew install cmake pkg-config sdl2 ffmpeg
```

Debian/Ubuntu:

```sh
sudo apt install build-essential cmake pkg-config libsdl2-dev \
    libavformat-dev libavcodec-dev libswscale-dev libavutil-dev
```

Then:

```sh
cmake -S . -B build
cmake --build build -j
cmake --install build --prefix ~/.local   # puts vcrop on PATH
```

## Encoding defaults

Cropping requires re-encoding the video stream. The generated command uses
`libx264 -crf 18 -preset veryfast` (visually near-lossless) and copies the
audio stream unchanged. Use **Copy command** and edit the command if you
need different settings.
