#!/usr/bin/env bash
# Build vcrop and expose it as a `vcrop` command on PATH. Also registers
# vcrop in file managers' "Open With" menus for video files:
#   - Linux: a desktop entry in ~/.local/share/applications (Dolphin, GNOME
#     Files, ...)
#   - macOS: an app bundle in ~/Applications registered with Launch Services
#     (Finder)
#
# Installs into ~/.local/bin (creates it if needed). The built binary is
# symlinked, so rebuilding via this script updates the command in place.
set -euo pipefail

repo_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
build_dir="$repo_dir/build"
bin_dir="$HOME/.local/bin"
bin_path="$bin_dir/vcrop"
os="$(uname -s)"

echo "==> configuring release build in $build_dir"
cmake -S "$repo_dir" -B "$build_dir" -DCMAKE_BUILD_TYPE=Release

echo "==> building"
cmake --build "$build_dir" -j"$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)"

echo "==> installing to $bin_path"
mkdir -p "$bin_dir"
ln -sf "$build_dir/vcrop" "$bin_path"

if [[ ":$PATH:" != *":$bin_dir:"* ]]; then
    echo
    echo "note: $bin_dir is not on your PATH, so \`vcrop\` will not resolve yet."
    case "${SHELL##*/}" in
        bash) rc="$HOME/.bashrc" ;;
        zsh)  rc="$HOME/.zshrc" ;;
        *)    rc="" ;;
    esac
    if [[ -n "$rc" && -w "$rc" ]]; then
        printf 'add "%s" to %s? [y/N] ' "$bin_dir" "$rc"
        read -r answer
        if [[ "$answer" =~ ^[Yy]$ ]]; then
            printf '\nexport PATH="%s:$PATH"\n' "$bin_dir" >> "$rc"
            echo "added. Run \`source $rc\` or open a new shell."
        else
            echo "add the following line to your shell rc file manually:"
            echo "  export PATH=\"$bin_dir:\$PATH\""
        fi
    else
        echo "add the following line to your shell rc file manually:"
        echo "  export PATH=\"$bin_dir:\$PATH\""
    fi
fi

# Dolphin's inline "Open With" menu is built from the [Added Associations]
# section of ~/.config/mimeapps.list; merely declaring MimeType= in the
# desktop entry only gets an app into "Other Applications". Merge vcrop into
# the associations for every video MIME type the system knows about, without
# touching [Default Applications].
add_mime_associations() {
    local apps_file="$HOME/.config/mimeapps.list"
    local types_file tmp
    types_file="$(mktemp)"
    tmp="$(mktemp)"

    # Video MIME types known to the system: /usr/share/mime/video/*.xml,
    # plus any video/* keys already listed in mimeapps.list (covers types
    # without an XML entry).
    {
        for f in /usr/share/mime/video/*.xml; do
            [[ -f "$f" ]] && printf 'video/%s\n' "$(basename "$f" .xml)"
        done
        if [[ -f "$apps_file" ]]; then
            sed -n 's/^\(video\/[^=]*\)=.*/\1/p' "$apps_file"
        fi
    } | sort -u > "$types_file"

    if [[ ! -f "$apps_file" ]]; then
        mkdir -p "$(dirname "$apps_file")"
        {
            printf '[Added Associations]\n'
            while read -r t; do
                printf '%s=vcrop.desktop;\n' "$t"
            done < "$types_file"
        } > "$apps_file"
    else
        awk -v types_file="$types_file" '
            BEGIN {
                while ((getline t < types_file) > 0) video_types[t] = 1
                section = ""
                pending = 0
            }
            function flush_pending() {
                for (t in video_types) print t "=vcrop.desktop;"
                pending = 0
            }
            /^\[/ {
                if (pending) flush_pending()
                section = $0
                print
                next
            }
            {
                if (section == "[Added Associations]") {
                    pending = 1
                    eq = index($0, "=")
                    key = substr($0, 1, eq - 1)
                    if (key in video_types) {
                        delete video_types[key]
                        val = substr($0, eq + 1)
                        if (val ~ /(^|;)vcrop\.desktop(;|$)/) { print; next }
                        sub(/;$/, "", $0)
                        print $0 ";vcrop.desktop;"
                        next
                    }
                }
                print
            }
            END { if (pending) flush_pending() }
        ' "$apps_file" > "$tmp" && mv "$tmp" "$apps_file"
    fi
    rm -f "$types_file" "$tmp"
    echo "registered vcrop in: $apps_file"
}

install_open_with() {
    case "$os" in
        Linux)
            echo "==> registering in file managers (Open With)"
            desktop_dir="$HOME/.local/share/applications"
            desktop_path="$desktop_dir/vcrop.desktop"
            mkdir -p "$desktop_dir"
            # Dolphin launches via klauncher, which does not read shell rc
            # files, so use the absolute path to the binary.
            sed "s|__VCROP_BIN__|$bin_path|" "$repo_dir/packaging/vcrop.desktop" \
                > "$desktop_path"
            echo "installed desktop entry: $desktop_path"
            if command -v desktop-file-validate >/dev/null 2>&1; then
                desktop-file-validate "$desktop_path"
            fi
            # Icon (named, so any icon theme engine can find it). hicolor's
            # index.theme only declares sizes up to 512x512, scalable and
            # symbolic, so 1024x1024 would be invisible to KDE; install into
            # declared directories instead (512x512@2x holds the 1024px
            # image for HiDPI).
            hicolor_dir="$HOME/.local/share/icons/hicolor"
            for sub in "512x512/apps" "512x512@2x/apps" "scalable/apps"; do
                mkdir -p "$hicolor_dir/$sub"
            done
            cp "$repo_dir/packaging/icons/vcrop.png" "$hicolor_dir/512x512/apps/vcrop.png"
            cp "$repo_dir/packaging/icons/vcrop.png" "$hicolor_dir/512x512@2x/apps/vcrop.png"
            cp "$repo_dir/packaging/icons/vcrop.svg" "$hicolor_dir/scalable/apps/vcrop.svg"
            echo "installed icons: $hicolor_dir/{512x512,512x512@2x,scalable}/apps"
            if command -v gtk-update-icon-cache >/dev/null 2>&1; then
                gtk-update-icon-cache -f -q "$hicolor_dir" \
                    2>/dev/null || true
            fi
            add_mime_associations
            if command -v kbuildsycoca6 >/dev/null 2>&1; then
                kbuildsycoca6 >/dev/null || true
                echo "refreshed KDE service cache"
            else
                echo "note: restart Dolphin (or run kbuildsycoca6) to pick up the new entry."
            fi
            ;;
        Darwin)
            echo "==> registering with Finder (Open With)"
            app_dir="$HOME/Applications"
            app_path="$app_dir/vcrop.app"
            mkdir -p "$app_path/Contents/MacOS" "$app_path/Contents/Resources"
            cp "$build_dir/vcrop" "$app_path/Contents/MacOS/vcrop"
            cp "$repo_dir/packaging/macos/Info.plist" "$app_path/Contents/Info.plist"
            # Build vcrop.icns from the shared 1024px source PNG using only
            # tools built into macOS (sips resizes, iconutil wraps).
            iconset="$(mktemp -d)/vcrop.iconset"
            mkdir -p "$iconset"
            for spec in "16:16x16" "32:16x16@2x" "32:32x32" "64:32x32@2x" \
                        "128:128x128" "256:128x128@2x" "256:256x256" \
                        "512:256x256@2x" "512:512x512" "1024:512x512@2x"; do
                size="${spec%%:*}"
                name="${spec##*:}"
                sips -z "$size" "$size" "$repo_dir/packaging/icons/vcrop.png" \
                    --out "$iconset/icon_${name}.png" >/dev/null
            done
            iconutil -c icns "$iconset" -o "$app_path/Contents/Resources/vcrop.icns"
            rm -rf "$(dirname "$iconset")"
            echo "built bundle icon: $app_path/Contents/Resources/vcrop.icns"
            lsregister="/System/Library/Frameworks/CoreServices.framework/Frameworks/LaunchServices.framework/Support/lsregister"
            "$lsregister" -f "$app_path"
            echo "registered app bundle: $app_path"
            ;;
        *)
            echo "note: no \"Open With\" registration for $os (Linux and macOS are supported)."
            ;;
    esac
}

install_open_with

echo
echo "installed. Try: vcrop --help"
