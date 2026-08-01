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

## Install / Uninstall

```sh
./install.sh      # builds and puts `vcrop` on PATH (~/.local/bin)
./uninstall.sh    # removes the command, build artifacts, the Open With
                  # registration, and install.sh's PATH line
```

`install.sh` builds a release binary into `build/` and symlinks it into
`~/.local/bin`, so re-running it after changes simply rebuilds and updates
the link. If `~/.local/bin` is not on your PATH it offers to add it to your
shell rc file.

### Open With (file managers)

`install.sh` also registers vcrop so it shows up under *Open With* for video
files (it never becomes the default player):

- **Linux (Dolphin, GNOME Files, ...):** a desktop entry is written to
  `~/.local/share/applications/vcrop.desktop` declaring `video/*`, and vcrop
  is added to the `[Added Associations]` of `~/.config/mimeapps.list` for
  every video MIME type on the system, so it appears directly in the *Open
  With* submenu. The KDE service cache is refreshed (`kbuildsycoca6`); if a
  menu entry does not appear immediately, restart Dolphin.
- **macOS (Finder):** the binary is wrapped in a `vcrop.app` bundle
  installed to `~/Applications` and registered with Launch Services. Right
  click a video → *Open With* → vcrop.

The icon (file-manager menus and the app window) comes from
`packaging/icons/vcrop.png`. To use your own, replace that file and
re-run `./install.sh` (menus) plus a rebuild for the window icon
(`./install.sh` rebuilds, or `cmake --build build`).

Note for macOS: if vcrop is already running when a file is opened from
Finder, Launch Services delivers the file to the running instance as an
Apple event, which the binary ignores. Quit vcrop first.

Note for Wayland: SDL2 cannot set window icons under Wayland; the taskbar
icon there is shown from the desktop entry instead, which ships the same
image.

## Building

Dependencies: CMake ≥ 3.24, a C++17 compiler, pkg-config, and the FFmpeg
development libraries. SDL2 and Dear ImGui are vendored: pinned release
tarballs are downloaded and built by CMake on the first configure, so no
system installs of those are needed.

Arch Linux (also CachyOS / EndeavourOS / Manjaro):

```sh
sudo pacman -S --needed base-devel cmake pkg-config ffmpeg \
    libx11 libxext libxrandr libxinerama libxcursor libxi
```

The `libx*` packages are needed on X11 systems to build SDL2's video
drivers; Wayland-only users need `wayland` instead. macOS:

```sh
brew install cmake pkg-config ffmpeg
```

Then, to build without installing:

```sh
cmake -S . -B build
cmake --build build -j
```

or use `./install.sh` to build and put `vcrop` on PATH.

## Encoding defaults

Cropping requires re-encoding the video stream. The generated command uses
`libx264 -crf 18 -preset veryfast` (visually near-lossless) and copies the
audio stream unchanged. Use **Copy command** and edit the command if you
need different settings.
