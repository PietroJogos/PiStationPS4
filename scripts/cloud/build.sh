#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
set -euo pipefail
script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
source "$script_dir/env.sh"
source "$script_dir/upstream.env"
source_dir="${PISTATION_SOURCE_DIR:-$PISTATION_ENV_DIR/source/shadPS4}"
build_dir="${PISTATION_BUILD_DIR:-$PISTATION_ENV_DIR/build/baseline}"
jobs="${PISTATION_JOBS:-2}"
mkdir -p "$PISTATION_ENV_DIR/logs"
export XDG_DATA_HOME="$PISTATION_ENV_DIR/state/data"
export XDG_CONFIG_HOME="$PISTATION_ENV_DIR/state/config"
export XDG_CACHE_HOME="$PISTATION_ENV_DIR/state/cache"
mkdir -p "$XDG_DATA_HOME" "$XDG_CONFIG_HOME" "$XDG_CACHE_HOME"
if [[ ! -f "$PISTATION_ENV_DIR/ready" ]]; then
    echo 'Execute scripts/cloud/setup.sh primeiro.' >&2
    exit 1
fi
if [[ ! -f "$source_dir/CMakeLists.txt" || "$(git -C "$source_dir" rev-parse HEAD)" != "$UPSTREAM_COMMIT" ]]; then
    echo 'Execute scripts/cloud/prepare-source.sh primeiro.' >&2
    exit 1
fi
git -C "$source_dir" submodule status --recursive > "$PISTATION_ENV_DIR/logs/submodule-status.txt"
if grep -Eq '^[+U-]' "$PISTATION_ENV_DIR/logs/submodule-status.txt"; then
    echo 'Submódulos incompletos ou divergentes. Execute prepare-source.sh.' >&2
    exit 1
fi
git -C "$source_dir" diff --exit-code --ignore-submodules=none
git -C "$source_dir" diff --cached --exit-code --ignore-submodules=none
cmake -S "$source_dir" -B "$build_dir" -G Ninja \
    -DCMAKE_C_COMPILER="$CC" -DCMAKE_CXX_COMPILER="$CXX" \
    -DCMAKE_SYSROOT="$PISTATION_PREFIX" \
    "-DCMAKE_C_FLAGS=--gcc-toolchain=$PISTATION_PREFIX/usr" \
    "-DCMAKE_CXX_FLAGS=--gcc-toolchain=$PISTATION_PREFIX/usr" \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
    -DENABLE_SYSTEM_LIBRARIES=OFF -DENABLE_TESTS=OFF \
    2>&1 | tee "$PISTATION_ENV_DIR/logs/configure.log"
cmake --build "$build_dir" --parallel "$jobs" \
    2>&1 | tee "$PISTATION_ENV_DIR/logs/build.log"
ldd "$build_dir/shadps4" | tee "$PISTATION_ENV_DIR/logs/ldd.log"
if grep -q 'not found' "$PISTATION_ENV_DIR/logs/ldd.log"; then
    exit 1
fi
"$build_dir/shadps4" --help 2>&1 | tee "$PISTATION_ENV_DIR/logs/help.log"
