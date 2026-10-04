# usbntfs

Read-only NTFS for USB drives in Nintendo Switch homebrew, without GPL code.

libusbhsfs' ISC build reads FAT and exFAT only; its NTFS comes from NTFS-3G,
which is GPL. usbntfs adds NTFS to the ISC build, read-only, on top of the
[`ntfs`](https://github.com/ColinFinck/ntfs) crate (MIT/Apache-2.0):

- `src/lib.rs` — the Rust library (`no_std` + `alloc`, C `malloc`), with a C
  interface (`include/usbntfs.h`): mount from a sector callback, stat, list
  directories, read file data.
- `libusbhsfs/` — a devoptab over it and an anchored patch for libusbhsfs
  v0.2.9, guarded by `USBNTFS_BUILD`. NTFS partitions then mount as `umsN:`
  like FAT ones; every write fails with `EROFS`, and the device is reported
  as NTFS and write-protected.

## Building

```sh
# 1. libusbntfs.a — nightly Rust with rust-src (image: devkitpro-mesa-rust)
scripts/build_switch.sh
# 2. libusbhsfs.a with NTFS — devkitPro (image: ghcr.io/autorunhq/switch-dev)
scripts/build_libusbhsfs.sh
```

Both land in `dist/`. Link `-lusbhsfs -lusbntfs` with `dist/lib` ahead of
portlibs, so its `libusbhsfs.a` is found before the FAT-only one.

## Testing

On the host, against NTFS images made with `mkntfs`/`ntfs-3g`:

```sh
cargo build --release
cc -Iinclude tests/host_test.c target/release/libusbntfs.a -o host_test && ./host_test test.img big.bin
cc -Iinclude tests/tree_test.c target/release/libusbntfs.a -o tree_test && ./tree_test big.img big.bin frag_a.ref
```

## Limits

- Read-only, by design.
- Names longer than 255 bytes in UTF-8 are skipped (newlib's `d_name` limit).
- Files stored compressed or encrypted (NTFS attributes) cannot be read:
  reads fail with EIO rather than return the stored bytes. Sparse files read
  normally.
- A volume left dirty by Windows (hibernation, Fast Startup) still mounts:
  nothing is written, so nothing can be damaged, but files changed since the
  last clean shutdown may read stale.

## License

usbntfs is MIT OR Apache-2.0. The libusbhsfs devoptab and patch are ISC, like
libusbhsfs. See `THIRD_PARTY_NOTICES.md` for what the built libraries contain.
