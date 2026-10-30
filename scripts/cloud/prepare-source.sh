#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
set -euo pipefail
script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
source "$script_dir/env.sh"
source "$script_dir/upstream.env"
source_dir="${PISTATION_SOURCE_DIR:-$PISTATION_ENV_DIR/source/shadPS4}"
mkdir -p "$PISTATION_ENV_DIR/logs"
if [[ ! -e "$source_dir" ]]; then
    mkdir -p "$source_dir"
    git -C "$source_dir" init
    git -C "$source_dir" remote add origin "$UPSTREAM_URL"
    git -C "$source_dir" fetch --depth 1 origin "$UPSTREAM_COMMIT"
    git -C "$source_dir" checkout --detach FETCH_HEAD
fi
if [[ "$(git -C "$source_dir" rev-parse HEAD)" != "$UPSTREAM_COMMIT" ]]; then
    echo "Revisão inesperada em $source_dir. Use outra PISTATION_SOURCE_DIR." >&2
    exit 1
fi
git -C "$source_dir" submodule update --init --recursive --depth 1 --jobs 2 \
    2>&1 | tee "$PISTATION_ENV_DIR/logs/submodules.log"
git -C "$source_dir" submodule status --recursive > "$PISTATION_ENV_DIR/logs/submodule-status.txt"
if grep -Eq '^[+U-]' "$PISTATION_ENV_DIR/logs/submodule-status.txt"; then
    echo 'Há submódulos ausentes ou em revisões diferentes das fixadas.' >&2
    exit 1
fi
git -C "$source_dir" diff --exit-code --ignore-submodules=none
git -C "$source_dir" diff --cached --exit-code --ignore-submodules=none
echo "Código original preparado em $source_dir"
