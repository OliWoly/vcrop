#!/usr/bin/env bash
# Remove vcrop: the installed command, build artifacts, any PATH line that
# install.sh added to a shell rc file, and the "Open With" registration
# (desktop entry on Linux, app bundle on macOS).
set -euo pipefail

repo_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
build_dir="$repo_dir/build"
bin_dir="$HOME/.local/bin"
bin_path="$bin_dir/vcrop"
os="$(uname -s)"
removed=0

remove_item() {
    local item="$1"
    if [[ -e "$item" || -L "$item" ]]; then
        rm -rf "$item"
        echo "removed: $item"
        removed=1
    fi
}

if [[ "$(readlink "$bin_path" 2>/dev/null || echo "")" == "$build_dir/vcrop" ]]; then
    rm -f "$bin_path"
    echo "removed: $bin_path"
    removed=1
else
    remove_item "$bin_path"
fi
if [[ -d "$bin_dir" ]] && ! ls -A "$bin_dir" | grep -q .; then
    rmdir "$bin_dir"
    echo "removed: $bin_dir (was empty)"
fi

remove_item "$repo_dir/build"
remove_item "$repo_dir/.cache"

# CLion-style build dirs, if they are CMake builds.
for d in "$repo_dir"/cmake-build-*; do
    [[ -d "$d" && -f "$d/CMakeCache.txt" ]] && remove_item "$d"
done

# Open With registrations.
if [[ "$os" == "Linux" ]]; then
    remove_item "$HOME/.local/share/applications/vcrop.desktop"
    if [[ -d "$HOME/.local/share/applications" ]] && ! ls -A "$HOME/.local/share/applications" | grep -q .; then
        rmdir "$HOME/.local/share/applications"
        echo "removed: $HOME/.local/share/applications (was empty)"
    fi
    hicolor_dir="$HOME/.local/share/icons/hicolor"
    for icon in \
        "$hicolor_dir/512x512/apps/vcrop.png" \
        "$hicolor_dir/512x512@2x/apps/vcrop.png" \
        "$hicolor_dir/scalable/apps/vcrop.svg"; do
        remove_item "$icon"
    done
    # Prune the dirs we created, but only if they are empty (hicolor is
    # shared with other apps).
    for d in \
        "$hicolor_dir/512x512/apps" "$hicolor_dir/512x512" \
        "$hicolor_dir/512x512@2x/apps" "$hicolor_dir/512x512@2x" \
        "$hicolor_dir/scalable/apps" "$hicolor_dir/scalable"; do
        [[ -d "$d" ]] && rmdir "$d" 2>/dev/null && echo "removed: $d (was empty)"
    done
    apps_file="$HOME/.config/mimeapps.list"
    if [[ -f "$apps_file" ]]; then
        tmp="$(mktemp)"
        awk '
            /^\[/ { section = $0; print; next }
            section == "[Added Associations]" && /^video\// {
                eq = index($0, "=")
                key = substr($0, 1, eq - 1)
                val = substr($0, eq + 1)
                gsub(/;?vcrop\.desktop/, "", val)
                gsub(/^;+|;+$/, "", val)
                if (val == "") { next }
                print key "=" val ";"
                next
            }
            { print }
        ' "$apps_file" > "$tmp" && mv "$tmp" "$apps_file"
        echo "removed vcrop from: $apps_file"
        removed=1
    fi
    command -v kbuildsycoca6 >/dev/null 2>&1 && kbuildsycoca6 >/dev/null || true
elif [[ "$os" == "Darwin" ]]; then
    app_path="$HOME/Applications/vcrop.app"
    if [[ -d "$app_path" ]]; then
        lsregister="/System/Library/Frameworks/CoreServices.framework/Frameworks/LaunchServices.framework/Support/lsregister"
        "$lsregister" -u "$app_path" >/dev/null 2>&1 || true
        remove_item "$app_path"
    fi
fi

# Undo the PATH line install.sh may have appended to a shell rc file.
line="export PATH=\"$bin_dir:\$PATH\""
for rc in "$HOME/.bashrc" "$HOME/.zshrc"; do
    if [[ -f "$rc" ]] && grep -Fqx "$line" "$rc"; then
        printf 'remove the PATH line install.sh added to %s? [y/N] ' "$rc"
        read -r answer
        if [[ "$answer" =~ ^[Yy]$ ]]; then
            grep -Fvx "$line" "$rc" > "$rc.tmp" && mv "$rc.tmp" "$rc"
            echo "removed from: $rc"
            removed=1
        else
            echo "left as-is: $rc"
        fi
    fi
done

if (( removed )); then
    echo
    echo "vcrop has been uninstalled."
else
    echo "nothing to uninstall."
fi
