#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
set -euo pipefail
script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
source "$script_dir/env.sh"
for tool in cmake ninja "$CC" "$CXX" gcc g++ ld pkg-config; do
    command -v "$tool"
    "$tool" --version | head -n 1
done
python3 - <<'PY'
import subprocess
version = subprocess.check_output(['cmake', '--version'], text=True).split()[2]
assert tuple(map(int, version.split('.')[:2])) >= (3, 24), version
PY
probe_dir="$(mktemp -d "$PISTATION_ENV_DIR/probe.XXXXXX")"
trap 'rm -rf -- "$probe_dir"' EXIT
cat > "$probe_dir/probe.cpp" <<'CPP'
#include <expected>
#include <format>
#include <string>
int main() {
    std::expected<int, std::string> value = 900;
    return std::format("{}", value.value()) == "900" ? 0 : 1;
}
CPP
"$CXX" --sysroot="$PISTATION_PREFIX" --gcc-toolchain="$PISTATION_PREFIX/usr" \
    -std=c++23 -march=x86-64-v3 "$probe_dir/probe.cpp" -o "$probe_dir/probe"
"$probe_dir/probe"
pkg-config --modversion alsa libpulse libudev libevdev x11 wayland-client vulkan
echo 'Ferramentas, bibliotecas e compilação C++23 verificadas.'
