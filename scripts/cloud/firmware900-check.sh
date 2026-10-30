#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
set -euo pipefail
script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
source "$script_dir/env.sh"
if [[ ! -f "$PISTATION_ENV_DIR/ready" ]]; then
    echo 'Execute scripts/cloud/setup.sh primeiro.' >&2
    exit 1
fi
build_dir="$PISTATION_ENV_DIR/build/firmware900"
mkdir -p "$PISTATION_ENV_DIR/logs"
cmake -S "$PISTATION_ROOT/tools/firmware900" -B "$build_dir" -G Ninja \
    -DCMAKE_CXX_COMPILER="$CXX" -DCMAKE_BUILD_TYPE=Debug \
    "-DPISTATION_NATIVE_REGISTRY_TESTS=${PISTATION_NATIVE_REGISTRY_TESTS:-OFF}" \
    -DCMAKE_SYSROOT="$PISTATION_PREFIX" \
    "-DCMAKE_CXX_FLAGS=--gcc-toolchain=$PISTATION_PREFIX/usr" \
    2>&1 | tee "$PISTATION_ENV_DIR/logs/scanner-configure.log"
cmake --build "$build_dir" --parallel "${PISTATION_JOBS:-2}" \
    2>&1 | tee "$PISTATION_ENV_DIR/logs/scanner-build.log"
ctest --test-dir "$build_dir" --output-on-failure \
    2>&1 | tee "$PISTATION_ENV_DIR/logs/scanner-tests.log"
"$build_dir/pistation-firmware900" --help \
    2>&1 | tee "$PISTATION_ENV_DIR/logs/scanner-help.log"
