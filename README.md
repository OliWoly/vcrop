# VCrop

Crop and trim a video from the file manager's right-click menu.

Right-click a video in Dolphin (KDE Plasma) and choose **Crop / Trim Video…**, or
use **Open With → VCrop** in Finder. The video opens in its own window, where you
drag a rectangle to crop, set a start and end, and export.

Qt 6 does the playback and UI; the export links the system FFmpeg libraries
(libavformat, libavcodec, libavfilter) directly instead of shelling out to `ffmpeg`.

## Build and install

### Arch Linux (KDE Plasma)

```sh
sudo pacman -S --needed base-devel cmake qt6-base qt6-multimedia qt6-multimedia-ffmpeg ffmpeg

cmake -B build -DCMAKE_INSTALL_PREFIX="$HOME/.local"
cmake --build build -j
cmake --install build
```

Set the prefix at configure time, as above: the menu entries are generated with the
absolute path of the installed binary. Use `-DCMAKE_INSTALL_PREFIX=/usr` with
`sudo cmake --install build` for a system-wide install.

This installs:

| File | Purpose |
| --- | --- |
| `bin/vcrop` | the application |
| `share/kio/servicemenus/vcrop.desktop` | Dolphin's right-click entry for `video/*` |
| `share/applications/vcrop.desktop` | launcher entry and "Open With" |
| `share/icons/hicolor/scalable/apps/vcrop.svg` | icon |

If the entry does not show up straight away, restart Dolphin or run `kbuildsycoca6`.

### macOS

```sh
brew install cmake pkg-config qtmultimedia ffmpeg

cmake -B build -DCMAKE_PREFIX_PATH="$(brew --prefix)"
cmake --build build -j
cmake --install build --prefix ~/Applications
```

`VCrop.app` declares itself as a handler for video files, so it appears under
**Open With** when you right-click a video in Finder.

Requires Qt 6.5+ and FFmpeg 7.1+.

### Uninstall

```sh
packaging/uninstall.sh          # or: cmake --build build --target uninstall
```

It looks in every standard install location (`~/.local`, `/usr/local`, `/usr`;
`~/Applications`, `/Applications` and anywhere Spotlight finds `VCrop.app`), lists
what it found and asks before removing it. Along with the installed files it
removes:

- the file dialog's remembered folder (`~/.config/vcroprc`, or
  `~/Library/Preferences/local.vcrop.plist`),
- preview copies left in the temp directory if VCrop crashed,
- the "Open With" registration on macOS. On Linux it refreshes the desktop, icon
  and Dolphin menu caches.

`-n` only lists, `-y` skips the question, and `--prefix DIR` adds a non-standard
install location. Run it as yourself first; if a system-wide install is left over,
it prints the `sudo` command that removes it. Files that belong to a distribution
package are left alone, and so is the build tree.

## Using it

| Action | How |
| --- | --- |
| Crop | Drag on the video. Drag inside the rectangle to move it, its edges or corners to resize. |
| Scrub | Click or drag on the bar at the top of the timeline. |
| Trim (cut a section out) | Drag across the lane under the bar, or press `I` where the section starts and `O` where it ends. Repeat for as many sections as you like. |
| Trim the beginning | Go to where the video should start and press `O`. |
| Adjust a trim | Drag its block to move it, or its edges to resize it. Select it (click it or its row in the list) to type exact times into the two fields. |
| Remove a trim | Select it and press `Delete`, or use **Remove** / **Clear all**. |
| Play / pause | `Space`. Playback jumps over the trims, so what plays is what the export will contain. Start inside a trim to review what it removes. |
| Step | `←` `→` one second, `Shift` + arrows five seconds, `,` `.` one frame, `Home` / `End` |
| Mute | `M` |
| Open another file | `Ctrl+O`, or drop a file on the window |
| Export | `Ctrl+E` |

Every trim gets its own colour and is listed beside the video with its start and
end time; the line under the transport controls shows how much of the video is
kept.

The line under the video shows the pointer position in video pixels and in screen
pixels, and the size and position of the crop rectangle. The crop snaps to even
pixel values, which H.264 and H.265 require.

## Export

The trims are cut out and what is left is joined into one file, in a single pass.

Without a crop the video is **copied without re-encoding** by default: instant and
lossless, in whatever codec the source uses. A copied stream can only be cut on
keyframes, so each kept part begins at the last keyframe before the chosen point
(the status line says by how much) and may end a few frames early. A trim too
short to contain a keyframe cannot be cut this way at all; the export says which
one and asks for a re-encode.

Tick **Re-encode** for frame-exact cuts. A crop always re-encodes. Either way the
audio tracks are copied untouched (cut on the audio packet nearest to each cut, so
at most about 10 ms off), and one of two presets is used for the video:

| Preset | Encoder settings | Use it for |
| --- | --- | --- |
| **H.265 perceptually lossless (RF 20)** (default) | x265, CRF 20, `slow`; 10-bit sources stay 10-bit, HDR tags and mastering metadata are kept; `hvc1` tag in MP4/MOV | archiving: small files that look like the source, slow to encode |
| **Maximum compatibility (H.264)** | x264, CRF 18, `veryfast`, always 8-bit | files that must play anywhere; fast |

- **Hardware encoder** applies to the H.264 preset only: VideoToolbox on macOS, or
  NVENC / VA-API on Linux, falling back to x264 if none can be opened.
- Exports go to `.mp4`, `.mov` or `.mkv`. MP4 and MOV are written "web optimised"
  (index at the front). The file is written as `<name>.part` and renamed once it is
  complete.

## Limitations

- On Wayland an application cannot learn the pointer's position on the screen, so
  the readout shows the position inside the window there instead.
- The H.264 preset is always 8-bit, so HDR and 10-bit sources lose bit depth with it.
- Subtitle and data tracks are not carried over.
- Homebrew's Qt plays video through AVFoundation, which cannot read Matroska. When
  a file fails to load, VCrop copies its streams (no re-encoding) into a temporary
  `.mov` and previews that; exports still read the original. The copy is as large
  as the source and is deleted when the window closes. Codecs macOS cannot decode
  (VP9, AV1 on older Macs) still have no preview there.
