#!/usr/bin/env bash
# Removes VCrop and everything it leaves behind: the installed files, the
# file-manager integration, and the per-user state the app or the OS created.
#
#   packaging/uninstall.sh [-n] [-y] [--prefix DIR]...
#
# Every standard install location is searched; --prefix adds another one.
# Run it as your own user first: that removes the per-user state and lists any
# system-wide files, which then need a second run with sudo.

set -u

dry_run=0
assume_yes=0
prefixes=()

usage() {
    sed -n '2,9s/^# \{0,1\}//p' "$0"
    echo
    echo "  -n, --dry-run     only list what would be removed"
    echo "  -y, --yes         do not ask for confirmation"
    echo "      --prefix DIR  also look for an install under DIR"
}

while [ $# -gt 0 ]; do
    case "$1" in
        -n|--dry-run) dry_run=1 ;;
        -y|--yes) assume_yes=1 ;;
        --prefix) [ $# -ge 2 ] || { usage >&2; exit 2; }; prefixes+=("${2%/}"); shift ;;
        --prefix=*) p=${1#--prefix=}; prefixes+=("${p%/}") ;;
        -h|--help) usage; exit 0 ;;
        *) usage >&2; exit 2 ;;
    esac
    shift
done

is_root=0
[ "$(id -u)" -eq 0 ] && is_root=1
os=$(uname -s)
src_dir=$(cd "$(dirname "$0")/.." && pwd)

targets=()      # paths to delete
unregister=()   # macOS bundles to drop from Launch Services first

add() { [ -e "$1" ] || [ -L "$1" ] && targets+=("$1"); }

contains() {
    local needle=$1 x; shift
    for x in "$@"; do [ "$x" = "$needle" ] && return 0; done
    return 1
}

# A file a distribution package installed belongs to the package manager.
packaged() {
    if command -v pacman >/dev/null 2>&1; then pacman -Qoq "$1" >/dev/null 2>&1 && return 0; fi
    if command -v dpkg >/dev/null 2>&1; then dpkg -S "$1" >/dev/null 2>&1 && return 0; fi
    if command -v rpm >/dev/null 2>&1; then rpm -qf "$1" >/dev/null 2>&1 && return 0; fi
    return 1
}

if [ "$os" = Darwin ]; then
    lsregister=/System/Library/Frameworks/CoreServices.framework/Frameworks/LaunchServices.framework/Support/lsregister
    bundle_id=local.vcrop

    candidates=("/Applications/VCrop.app")
    [ $is_root -eq 0 ] && candidates+=("$HOME/Applications/VCrop.app")
    for p in ${prefixes[@]+"${prefixes[@]}"}; do candidates+=("$p/VCrop.app"); done
    # Spotlight finds copies installed anywhere else.
    while IFS= read -r app; do
        [ -n "$app" ] && candidates+=("$app")
    done < <(mdfind "kMDItemCFBundleIdentifier == '$bundle_id'" 2>/dev/null)

    for app in ${candidates[@]+"${candidates[@]}"}; do
        [ -d "$app" ] || continue
        id=$(/usr/libexec/PlistBuddy -c 'Print :CFBundleIdentifier' "$app/Contents/Info.plist" 2>/dev/null)
        [ "$id" = "$bundle_id" ] || continue
        case "$app" in "$src_dir"/*) continue ;; esac   # the build tree is not an install
        contains "$app" ${targets[@]+"${targets[@]}"} && continue
        targets+=("$app")
        unregister+=("$app")
    done

    if [ $is_root -eq 0 ]; then
        lib="$HOME/Library"
        add "$lib/Preferences/$bundle_id.plist"   # file dialog state
        add "$lib/Saved Application State/$bundle_id.savedState"
        add "$lib/Caches/$bundle_id"
        add "$lib/HTTPStorages/$bundle_id"
        add "$lib/WebKit/$bundle_id"
        add "$lib/Application Support/vcrop"
        add "$lib/Caches/vcrop"
    fi
else
    candidates=(/usr/local /usr)
    [ $is_root -eq 0 ] && candidates=("$HOME/.local" "${candidates[@]}")
    candidates+=(${prefixes[@]+"${prefixes[@]}"})

    linux_prefixes=()
    for p in "${candidates[@]}"; do
        p=${p%/}
        contains "$p" ${linux_prefixes[@]+"${linux_prefixes[@]}"} && continue
        linux_prefixes+=("$p")
        for f in bin/vcrop \
                 share/applications/vcrop.desktop \
                 share/kio/servicemenus/vcrop.desktop \
                 share/icons/hicolor/scalable/apps/vcrop.svg; do
            [ -e "$p/$f" ] || continue
            if packaged "$p/$f"; then
                echo "Skipping $p/$f: it belongs to a distribution package; remove that instead."
                continue
            fi
            targets+=("$p/$f")
        done
    done

    if [ $is_root -eq 0 ]; then
        config="${XDG_CONFIG_HOME:-$HOME/.config}"
        add "$config/vcroprc"   # KDE file dialog state
        add "$config/vcrop"
        add "${XDG_CACHE_HOME:-$HOME/.cache}/vcrop"
        add "${XDG_DATA_HOME:-$HOME/.local/share}/vcrop"
    fi
fi

# Preview copies (QTemporaryDir, "<app name>-XXXXXX") outlive a crash.
if [ $is_root -eq 0 ]; then
    tmp="${TMPDIR:-/tmp}"
    for d in "${tmp%/}"/vcrop-??????; do
        [ -d "$d" ] && [ -O "$d" ] && targets+=("$d")
    done
fi

if [ ${#targets[@]} -eq 0 ]; then
    echo "Nothing of VCrop was found."
    [ $is_root -eq 1 ] && echo "(Per-user files are only looked for when this runs without sudo.)"
    exit 0
fi

echo "VCrop files found:"
printf '  %s\n' "${targets[@]}"

if [ $dry_run -eq 1 ]; then
    exit 0
fi

if pgrep -x vcrop >/dev/null 2>&1 || pgrep -x VCrop >/dev/null 2>&1; then
    echo "VCrop is running. Quit it and run this again." >&2
    exit 1
fi

if [ $assume_yes -eq 0 ]; then
    if [ ! -t 0 ]; then
        echo "Not a terminal; pass --yes to remove without asking." >&2
        exit 1
    fi
    printf 'Remove them? [y/N] '
    read -r answer
    case "$answer" in [yY]|[yY][eE][sS]) ;; *) echo "Nothing removed."; exit 1 ;; esac
fi

if [ "$os" = Darwin ]; then
    # Drop the app from Launch Services first, or Finder keeps offering it
    # under "Open With" until the next rebuild of its database.
    for app in ${unregister[@]+"${unregister[@]}"}; do
        [ -w "$(dirname "$app")" ] && "$lsregister" -u "$app" >/dev/null 2>&1
    done
    # cfprefsd caches preferences and would write the file straight back.
    [ $is_root -eq 0 ] && defaults delete local.vcrop >/dev/null 2>&1
fi

failed=()
removed_system=0
for t in "${targets[@]}"; do
    if rm -rf -- "$t" 2>/dev/null && [ ! -e "$t" ]; then
        echo "Removed $t"
        case "$t" in "$HOME"/*) ;; *) removed_system=1 ;; esac
    else
        failed+=("$t")
    fi
done

if [ "$os" != Darwin ]; then
    # Leave no empty service menu directories behind, then refresh the menus.
    for p in "${linux_prefixes[@]}"; do
        rmdir "$p/share/kio/servicemenus" "$p/share/kio" 2>/dev/null
        if [ -w "$p/share/applications" ] && command -v update-desktop-database >/dev/null 2>&1; then
            update-desktop-database -q "$p/share/applications" 2>/dev/null
        fi
        if [ -f "$p/share/icons/hicolor/index.theme" ] && [ -w "$p/share/icons/hicolor" ] \
                && command -v gtk-update-icon-cache >/dev/null 2>&1; then
            gtk-update-icon-cache -q -t "$p/share/icons/hicolor" 2>/dev/null
        fi
    done
    # The KDE menu cache is per user, so rebuilding it as root does nothing useful.
    if [ $is_root -eq 0 ] && command -v kbuildsycoca6 >/dev/null 2>&1; then
        kbuildsycoca6 >/dev/null 2>&1
    elif [ $removed_system -eq 1 ]; then
        echo "Restart Dolphin (or run kbuildsycoca6 as your user) to drop the menu entry."
    fi
fi

if [ ${#failed[@]} -gt 0 ]; then
    echo
    echo "Could not remove:"
    printf '  %s\n' "${failed[@]}"
    if [ $is_root -eq 0 ]; then
        again="sudo $0 --yes"
        for p in ${prefixes[@]+"${prefixes[@]}"}; do again="$again --prefix '$p'"; done
        echo "Run again with sudo to remove system-wide files: $again"
    fi
    exit 1
fi

echo "VCrop is uninstalled."
