#!/usr/bin/env bash
# Configure + build + run the zapret core verifier using the user-local Qt6/CMake prefix.
# No sudo required. Override the prefix with ZAPRETDEV=/path/to/prefix if you moved it.
set -euo pipefail
P="${ZAPRETDEV:-$HOME/.local/zapretdev/usr}"
export LD_LIBRARY_PATH="$P/lib/x86_64-linux-gnu:${LD_LIBRARY_PATH:-}"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
"$P/bin/cmake" -S "$HERE" -B "$HERE/build" -DCMAKE_PREFIX_PATH="$P" -DCMAKE_BUILD_TYPE="${BUILD_TYPE:-Debug}"
"$P/bin/cmake" --build "$HERE/build"
( cd "$HERE/build" && ctest --output-on-failure )
