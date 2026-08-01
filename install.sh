#!/usr/bin/env bash
# Build vcrop and expose it as a `vcrop` command on PATH.
#
# Installs into ~/.local/bin (creates it if needed). The built binary is
# symlinked, so rebuilding via this script updates the command in place.
set -euo pipefail

repo_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
build_dir="$repo_dir/build"
bin_dir="$HOME/.local/bin"
bin_path="$bin_dir/vcrop"

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

echo
echo "installed. Try: vcrop --help"
