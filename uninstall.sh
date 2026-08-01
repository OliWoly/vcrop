#!/usr/bin/env bash
# Remove vcrop: the installed command, build artifacts, and any PATH line
# that install.sh added to a shell rc file.
set -euo pipefail

repo_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
bin_dir="$HOME/.local/bin"
bin_path="$bin_dir/vcrop"
removed=0

remove_item() {
    local item="$1"
    if [[ -e "$item" || -L "$item" ]]; then
        rm -rf "$item"
        echo "removed: $item"
        removed=1
    fi
}

if [[ "$(readlink -f "$bin_path" 2>/dev/null)" == "$(readlink -f "$repo_dir/build/vcrop" 2>/dev/null)" ]]; then
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
