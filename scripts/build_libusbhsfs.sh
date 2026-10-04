#!/bin/bash
# Builds libusbhsfs v0.2.9 (ISC: FAT/exFAT) with read-only NTFS through
# usbntfs into dist/. Run after build_switch.sh, inside a devkitPro environment.
set -e
cd "$(dirname "$0")/.."
ROOT="$PWD"
REV=b1ff8811a762adf1b585b762b21308d458d43bfa # v0.2.9
SRC="$ROOT/target/libusbhsfs"
if [ ! -d "$SRC/.git" ] || [ "$(git -C "$SRC" rev-parse HEAD)" != "$REV" ]; then
    rm -rf "$SRC"
    git clone -q https://github.com/DarkMatterCore/libusbhsfs.git "$SRC"
    git -C "$SRC" checkout -q "$REV"
fi
git -C "$SRC" checkout -q -- . && git -C "$SRC" clean -qfdx
python3 libusbhsfs/apply.py "$SRC"
make -C "$SRC" BUILD_TYPE=ISC USBNTFS_INCLUDE="$ROOT/include" release -j"$(nproc)"
mkdir -p dist/lib dist/include
cp "$SRC/lib/libusbhsfs.a" dist/lib/
cp "$SRC/include/usbhsfs.h" dist/include/
echo "dist/lib/libusbhsfs.a (link with dist/lib/libusbntfs.a)"
