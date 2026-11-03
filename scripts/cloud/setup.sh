#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
set -euo pipefail
script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
source "$script_dir/env.sh"
source /etc/os-release
if [[ "$ID" != debian || "$VERSION_ID" != 13 || "$(uname -m)" != x86_64 ]]; then
    echo 'Este instalador portátil exige Debian 13 x86-64.' >&2
    exit 1
fi
for tool in curl gpg python3 dpkg-deb /usr/bin/apt-get; do
    command -v "$tool" >/dev/null
done
mkdir -p "$PISTATION_ENV_DIR"/{apt/lists/partial,apt/archives/partial,apt/gnupg,logs,runtime-lib,source} "$PISTATION_PREFIX"
rm -f "$PISTATION_ENV_DIR/ready"
apt_dir="$PISTATION_ENV_DIR/apt"
chmod 700 "$apt_dir/gnupg"
curl --fail --location --silent --show-error --max-time 120 \
    https://apt.llvm.org/llvm-snapshot.gpg.key -o "$apt_dir/llvm.asc"
fingerprint="$(gpg --homedir "$apt_dir/gnupg" --batch --show-keys --with-colons "$apt_dir/llvm.asc" | awk -F: '$1 == "fpr" && !seen++ {print $10}')"
if [[ "$fingerprint" != 6084F3CF814B57C1CF12EFD515CF4D18AF4F7421 ]]; then
    echo "Chave LLVM não reconhecida: $fingerprint" >&2
    exit 1
fi
gpg --homedir "$apt_dir/gnupg" --batch --yes --dearmor -o "$apt_dir/llvm.gpg" "$apt_dir/llvm.asc"
cat > "$apt_dir/sources.list" <<EOF
deb [signed-by=/usr/share/keyrings/debian-archive-keyring.gpg] https://deb.debian.org/debian trixie main
deb [signed-by=/usr/share/keyrings/debian-archive-keyring.gpg] https://deb.debian.org/debian trixie-updates main
deb [signed-by=/usr/share/keyrings/debian-archive-keyring.gpg] https://security.debian.org/debian-security trixie-security main
deb [signed-by=$apt_dir/llvm.gpg] https://apt.llvm.org/trixie/ llvm-toolchain-trixie-23 main
EOF
touch "$apt_dir/status"
apt_options=(
    -o "Dir::Etc::sourcelist=$apt_dir/sources.list"
    -o Dir::Etc::sourceparts=-
    -o "Dir::State::lists=$apt_dir/lists"
    -o "Dir::State::status=$apt_dir/status"
    -o "Dir::Cache::archives=$apt_dir/archives"
    -o "Dir::Cache::pkgcache=$apt_dir/pkgcache.bin"
    -o "Dir::Cache::srcpkgcache=$apt_dir/srcpkgcache.bin"
    -o Debug::NoLocking=1
    -o APT::Get::List-Cleanup=0
    -o APT::Update::Error-Mode=any
    -o APT::Sandbox::User="$(id -un)"
    -o Acquire::Retries=2
)
if [[ -n "${HTTPS_PROXY:-}" ]]; then
    apt_options+=(-o "Acquire::https::Proxy=$HTTPS_PROXY")
fi
packages=(
    build-essential clang-23 clang-format-23 lld-23 gdb cmake ninja-build
    pkg-config python3 git ca-certificates
    libasound2-dev libpulse-dev libopenal-dev libssl-dev zlib1g-dev libedit-dev
    libudev-dev libevdev-dev libsdl2-dev libjack-dev libsndio-dev
    libvulkan-dev vulkan-validationlayers vulkan-tools mesa-vulkan-drivers libpng-dev
    libx11-dev libxext-dev libwayland-dev libdecor-0-dev libxkbcommon-dev
    libxcursor-dev libxi-dev libxss-dev libxtst-dev libxrandr-dev libxfixes-dev
    uuid-dev libunwind-dev shellcheck
)
/usr/bin/apt-get "${apt_options[@]}" update 2>&1 | tee "$PISTATION_ENV_DIR/logs/apt-update.log"
/usr/bin/apt-get "${apt_options[@]}" --download-only --no-install-recommends -y \
    install "${packages[@]}" 2>&1 | tee "$PISTATION_ENV_DIR/logs/apt-download.log"
printf 'Package\tVersion\tArchitecture\n' > "$PISTATION_ENV_DIR/logs/packages.tsv"
for archive in "$apt_dir/archives/"*.deb; do
    dpkg-deb --extract "$archive" "$PISTATION_PREFIX"
    dpkg-deb --show --showformat="\${Package}\t\${Version}\t\${Architecture}\n" \
        "$archive" >> "$PISTATION_ENV_DIR/logs/packages.tsv"
done
python3 - "$PISTATION_PREFIX" "$PISTATION_ENV_DIR/runtime-lib" <<'PY'
import os
import pathlib
import sys

root = pathlib.Path(sys.argv[1])
runtime = pathlib.Path(sys.argv[2])
for name in ('bin', 'sbin', 'lib', 'lib64'):
    path = root / name
    if not path.exists() and not path.is_symlink():
        path.symlink_to('usr/' + name)
for path in root.rglob('*'):
    if path.is_symlink():
        target = os.readlink(path)
        local = root / target.lstrip('/')
        if target.startswith('/') and local.exists():
            path.unlink()
            path.symlink_to(os.path.relpath(local, path.parent))
glibc = ('ld-linux', 'libc.so', 'libm.so', 'libmvec.so', 'libpthread.so',
         'librt.so', 'libdl.so', 'libresolv.so', 'libnss_', 'libutil.so',
         'libanl.so', 'libBrokenLocale.so', 'libthread_db.so', 'libmemusage.so',
         'libpcprofile.so')
for directory in (root / 'usr/lib/x86_64-linux-gnu', root / 'usr/lib/llvm-23/lib'):
    for path in directory.glob('**/*.so*'):
        if path.is_file() and not path.name.startswith(glibc):
            link = runtime / path.name
            if link.is_symlink():
                link.unlink()
            link.symlink_to(os.path.relpath(path, runtime))
PY
(
    cd -- "$apt_dir/archives"
    sha256sum -- ./*.deb
) > "$PISTATION_ENV_DIR/logs/packages.sha256"
bash "$script_dir/check.sh"
touch "$PISTATION_ENV_DIR/ready"
