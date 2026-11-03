#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later

PISTATION_ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)"
export PISTATION_ENV_DIR="${PISTATION_ENV_DIR:-$PISTATION_ROOT/.cloud-env}"
export PISTATION_PREFIX="$PISTATION_ENV_DIR/sysroot"
export PATH="$PISTATION_PREFIX/usr/lib/llvm-23/bin:$PISTATION_PREFIX/usr/bin:$PATH"
export LD_LIBRARY_PATH="$PISTATION_ENV_DIR/runtime-lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export PKG_CONFIG_PATH="$PISTATION_PREFIX/usr/lib/x86_64-linux-gnu/pkgconfig:$PISTATION_PREFIX/usr/share/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
export PKG_CONFIG_SYSROOT_DIR="$PISTATION_PREFIX"
export CMAKE_PREFIX_PATH="$PISTATION_PREFIX/usr${CMAKE_PREFIX_PATH:+:$CMAKE_PREFIX_PATH}"
export CC="${CC:-clang-23}"
export CXX="${CXX:-clang++-23}"
