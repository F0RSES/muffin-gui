#!/usr/bin/env bash
# Configure + build + test the zapret GUI (core library, GUI app, headless tests).
# No sudo required. Override the prefix with ZAPRETDEV=/path/to/prefix if you moved it.
set -euo pipefail
P="${ZAPRETDEV:-$HOME/.local/zapretdev/usr}"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

# This prefix carries Qt headers/.prl files but no versioned runtime objects; make the CMake package
# files resolvable before configure runs (idempotent, only creates missing symlinks).
"$HERE/scripts/ensure_qt_prefix.sh"

export LD_LIBRARY_PATH="$P/lib/x86_64-linux-gnu:${LD_LIBRARY_PATH:-}"

# Qt platform/imageformat plugins (xcb, offscreen, svg) are not part of this prefix; point at the
# system set of the same Qt version so the GUI and the offscreen tests can both start.
PLUGINS="$("$HERE/scripts/ensure_qt_prefix.sh" --print-plugin-path || true)"
if [ -n "${PLUGINS:-}" ]; then
  export QT_PLUGIN_PATH="${QT_PLUGIN_PATH:-$PLUGINS}"
  echo "using Qt plugins from $QT_PLUGIN_PATH"
fi

"$P/bin/cmake" -S "$HERE" -B "$HERE/build" -DCMAKE_PREFIX_PATH="$P" -DCMAKE_BUILD_TYPE="${BUILD_TYPE:-Debug}"
"$P/bin/cmake" --build "$HERE/build"
( cd "$HERE/build" && ctest --output-on-failure )
