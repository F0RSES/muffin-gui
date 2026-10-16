#!/usr/bin/env bash
# Make a partially-installed user-local Qt prefix usable by CMake.
#
# The prefix under $ZAPRETDEV (default ~/.local/zapretdev/usr) ships headers, .prl files and the
# libQt6*.so dev symlinks, but no versioned runtime objects (libQt6Gui.so.6 / .so.6.10.2). Qt6's
# CMake package files verify that every imported target's LOCATION file exists, so find_package
# fails on the first GUI module until those names resolve.
#
# This creates the missing names as symlinks to the *system* Qt runtime of the same version, and only
# touches files that are absent — running it repeatedly is a no-op.
set -euo pipefail

P="${ZAPRETDEV:-$HOME/.local/zapretdev/usr}"
LIBDIR="$P/lib/x86_64-linux-gnu"
SYS="${ZAPRET_SYS_QT_LIBDIR:-/usr/lib/x86_64-linux-gnu}"

[ -d "$LIBDIR" ] || { echo "ensure_qt_prefix: $LIBDIR does not exist" >&2; exit 1; }

if [ "$#" -gt 0 ] && [ "$1" = "--print-plugin-path" ]; then
    # Report a Qt plugin directory containing a platform plugin (for QT_PLUGIN_PATH at runtime).
    for d in "$P/plugins" "$P/share/qt6/plugins" "$SYS/qt6/plugins" /usr/lib/qt6/plugins; do
        if ls "$d"/platforms/libq*.so >/dev/null 2>&1; then
            echo "$d"
            exit 0
        fi
    done
    exit 0
fi

linked=0
for so in "$LIBDIR"/libQt6*.so; do
    [ -e "$so" ] || continue
    name="$(basename "$so" .so)"                       # libQt6Gui
    [ -e "$LIBDIR/$name.so.6" ] && continue            # already resolvable
    for cand in "$SYS/$name.so.6.10.2" "$SYS/$name.so.6"; do
        if [ -e "$cand" ]; then
            if [ "$cand" = "$SYS/$name.so.6.10.2" ]; then
                [ -e "$LIBDIR/$name.so.6.10.2" ] || ln -s "$cand" "$LIBDIR/$name.so.6.10.2"
                ln -sfn "$name.so.6.10.2" "$LIBDIR/$name.so.6"
            else
                ln -s "$cand" "$LIBDIR/$name.so.6"
            fi
            linked=$((linked + 1))
            break
        fi
    done
done

echo "ensure_qt_prefix: linked $linked missing runtime library name(s) in $LIBDIR"
