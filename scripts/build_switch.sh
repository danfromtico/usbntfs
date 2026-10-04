#!/bin/bash
# Builds libusbntfs.a for the Switch (tier-3 aarch64-nintendo-switch-freestanding,
# core + alloc built from source). Needs nightly Rust with rust-src.
set -e
cd "$(dirname "$0")/.."
RUSTFLAGS="-C target-cpu=cortex-a57" \
cargo +nightly build --release --target aarch64-nintendo-switch-freestanding \
    -Z build-std=core,alloc -Z build-std-features=
mkdir -p dist/lib dist/include
cp target/aarch64-nintendo-switch-freestanding/release/libusbntfs.a dist/lib/
cp include/usbntfs.h dist/include/
echo "dist/lib/libusbntfs.a"
