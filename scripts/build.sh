#!/usr/bin/env bash
# Configure + build + test the zapret GUI (core library, GUI app, headless tests).
# No sudo required. Two host shapes are supported:
#   1. A developer-local Qt prefix at $ZAPRETDEV (default ~/.local/zapretdev/usr). It ships headers and
#      .prl files but no versioned runtime objects, so ensure_qt_prefix.sh first makes the CMake
#      package files resolvable and the runtime/plugin paths are pointed at it.
#   2. No such prefix (a fresh clone, or CI that drives cmake itself): fall back to the system cmake
#      and the system Qt6, with no prefix fixups.
set -euo pipefail
PREFIX="${ZAPRETDEV:-$HOME/.local/zapretdev/usr}"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

CMAKE_ARGS=("-DCMAKE_BUILD_TYPE=${BUILD_TYPE:-Debug}")
if [ -x "$PREFIX/bin/cmake" ]; then
  CMAKE="$PREFIX/bin/cmake"
  CMAKE_ARGS+=("-DCMAKE_PREFIX_PATH=$PREFIX")

  # This prefix carries Qt headers/.prl files but no versioned runtime objects; make the CMake package
  # files resolvable before configure runs (idempotent, only creates missing symlinks).
  "$HERE/scripts/ensure_qt_prefix.sh"

  export LD_LIBRARY_PATH="$PREFIX/lib/x86_64-linux-gnu:${LD_LIBRARY_PATH:-}"

  # Qt platform/imageformat plugins (xcb, offscreen, svg) are not part of this prefix; point at the
  # system set of the same Qt version so the GUI and the offscreen tests can both start.
  PLUGINS="$("$HERE/scripts/ensure_qt_prefix.sh" --print-plugin-path || true)"
  if [ -n "${PLUGINS:-}" ]; then
    export QT_PLUGIN_PATH="${QT_PLUGIN_PATH:-$PLUGINS}"
    echo "using Qt plugins from $QT_PLUGIN_PATH"
  fi
  echo "using Qt prefix at $PREFIX"
else
  # No prefix on this host — use whatever cmake is on PATH and let find_package resolve the system Qt6.
  CMAKE="cmake"
  echo "no Qt prefix at $PREFIX — falling back to system cmake + system Qt6"
fi

"$CMAKE" -S "$HERE" -B "$HERE/build" "${CMAKE_ARGS[@]}"
"$CMAKE" --build "$HERE/build"
( cd "$HERE/build" && ctest --output-on-failure )
