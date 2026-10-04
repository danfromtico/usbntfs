//! Read-only NTFS for USB drives, as a C library.
//!
//! Built on the `ntfs` crate (MIT/Apache-2.0). The volume is read through a
//! sector callback supplied by the caller, so the same code serves a USB
//! drive on the Switch and a disk image in the host tests. Everything is
//! read-only: games are only ever read from a drive.
//!
//! Handles carry no borrows: files are kept by MFT record number and looked
//! up again on each read, and a directory listing is taken whole when it is
//! opened. A volume is not thread-safe; the caller serialises access to it.

#![no_std]

extern crate alloc;

use alloc::boxed::Box;
use alloc::string::String;
use alloc::vec::Vec;
use core::ffi::{c_char, c_int, c_void, CStr};
use core::ptr;

use binrw::io::{self, Read, Seek, SeekFrom};
use ntfs::indexes::NtfsFileNameIndex;
use ntfs::structured_values::NtfsFileNamespace;
use ntfs::{Ntfs, NtfsAttributeFlags, NtfsFile, NtfsReadSeek};

// ---------------------------------------------------------------------------
// Runtime: C allocator and abort on panic

extern "C" {
    fn malloc(size: usize) -> *mut c_void;
    fn free(ptr: *mut c_void);
    fn realloc(ptr: *mut c_void, size: usize) -> *mut c_void;
    fn aligned_alloc(align: usize, size: usize) -> *mut c_void;
    fn abort() -> !;
}

struct CAllocator;

unsafe impl core::alloc::GlobalAlloc for CAllocator {
    unsafe fn alloc(&self, layout: core::alloc::Layout) -> *mut u8 {
        if layout.align() <= 16 {
            malloc(layout.size()) as *mut u8
        } else {
            let size = (layout.size() + layout.align() - 1) & !(layout.align() - 1);
            aligned_alloc(layout.align(), size) as *mut u8
        }
    }
    unsafe fn dealloc(&self, ptr: *mut u8, _layout: core::alloc::Layout) {
        free(ptr as *mut c_void)
    }
    unsafe fn realloc(&self, ptr: *mut u8, layout: core::alloc::Layout, new_size: usize) -> *mut u8 {
        if layout.align() <= 16 {
            realloc(ptr as *mut c_void, new_size) as *mut u8
        } else {
            let new = self.alloc(core::alloc::Layout::from_size_align_unchecked(new_size, layout.align()));
            if !new.is_null() {
                ptr::copy_nonoverlapping(ptr, new, core::cmp::min(layout.size(), new_size));
                self.dealloc(ptr, layout);
            }
            new
        }
    }
}

#[global_allocator]
static ALLOCATOR: CAllocator = CAllocator;

#[panic_handler]
fn panic(_info: &core::panic::PanicInfo) -> ! {
    unsafe { abort() }
}

// ---------------------------------------------------------------------------
// Block device with a small read cache

/// Reads `count` sectors starting at `lba` into `buf`; true on success.
pub type ReadSectorsFn =
    unsafe extern "C" fn(user: *mut c_void, lba: u64, count: u32, buf: *mut u8) -> bool;

const CHUNK: usize = 64 * 1024;
const CHUNKS: usize = 8;

struct Device {
    read: ReadSectorsFn,
    user: *mut c_void,
    sector_size: u64,
    size: u64,
    pos: u64,
    // cached CHUNK-aligned pieces of the volume, least recently used first
    cache: Vec<(u64, Vec<u8>)>,
}

impl Device {
    fn chunk(&mut self, start: u64) -> io::Result<&[u8]> {
        if let Some(i) = self.cache.iter().position(|(s, _)| *s == start) {
            let entry = self.cache.remove(i);
            self.cache.push(entry);
        } else {
            let len = core::cmp::min(CHUNK as u64, self.size - start) as usize;
            let sectors = (len as u64).div_ceil(self.sector_size);
            let mut data = if self.cache.len() >= CHUNKS {
                self.cache.remove(0).1
            } else {
                Vec::new()
            };
            data.resize((sectors * self.sector_size) as usize, 0);
            let ok = unsafe {
                (self.read)(self.user, start / self.sector_size, sectors as u32, data.as_mut_ptr())
            };
            if !ok {
                return Err(io::Error::new(io::ErrorKind::Other, "sector read failed"));
            }
            data.truncate(len);
            self.cache.push((start, data));
        }
        Ok(&self.cache.last().unwrap().1)
    }
}

impl Read for Device {
    fn read(&mut self, buf: &mut [u8]) -> io::Result<usize> {
        if self.pos >= self.size || buf.is_empty() {
            return Ok(0);
        }
        let start = self.pos - self.pos % CHUNK as u64;
        let offset = (self.pos - start) as usize;
        let chunk = self.chunk(start)?;
        let n = core::cmp::min(buf.len(), chunk.len() - offset);
        buf[..n].copy_from_slice(&chunk[offset..offset + n]);
        self.pos += n as u64;
        Ok(n)
    }
}

impl Seek for Device {
    fn seek(&mut self, pos: SeekFrom) -> io::Result<u64> {
        let target = match pos {
            SeekFrom::Start(p) => p as i128,
            SeekFrom::End(d) => self.size as i128 + d as i128,
            SeekFrom::Current(d) => self.pos as i128 + d as i128,
        };
        if target < 0 {
            return Err(io::Error::new(io::ErrorKind::InvalidInput, "seek before start"));
        }
        self.pos = target as u64;
        Ok(self.pos)
    }
}

// ---------------------------------------------------------------------------
// Volume

pub struct Volume {
    dev: Device,
    ntfs: Ntfs,
}

/// What stat-like calls report.
#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct UsbNtfsStat {
    pub record: u64,
    pub size: u64,
    /// 100 ns intervals since 1601-01-01 (NT time)
    pub mtime: u64,
    pub is_dir: bool,
}

struct DirEntry {
    name: String,
    stat: UsbNtfsStat,
}

pub struct Dir {
    entries: Vec<DirEntry>,
    next: usize,
}

const ROOT_RECORD: u64 = 5;

impl Volume {
    /// The file a /-separated path names, from the root.
    fn lookup(&mut self, path: &str) -> Option<NtfsFile<'_>> {
        let Volume { dev, ntfs } = self;
        let mut file = ntfs.root_directory(dev).ok()?;
        for part in path.split('/').filter(|p| !p.is_empty()) {
            let index = file.directory_index(dev).ok()?;
            let mut finder = index.finder();
            let entry = NtfsFileNameIndex::find(&mut finder, ntfs, dev, part)?.ok()?;
            let next = entry.to_file(ntfs, dev).ok()?;
            drop(finder);
            file = next;
        }
        Some(file)
    }

    fn stat_of(dev: &mut Device, file: &NtfsFile<'_>) -> UsbNtfsStat {
        let is_dir = file.is_directory();
        let size = if is_dir {
            0
        } else {
            file.data(dev, "")
                .and_then(|item| item.ok())
                .and_then(|item| item.to_attribute().ok().and_then(|a| a.value(dev).ok().map(|v| v.len())))
                .unwrap_or(0)
        };
        let mtime = file.info().map(|i| i.modification_time().nt_timestamp()).unwrap_or(0);
        UsbNtfsStat { record: file.file_record_number(), size, mtime, is_dir }
    }

    fn list(&mut self, path: &str) -> Option<Vec<DirEntry>> {
        let record = self.lookup(path)?.file_record_number();
        let Volume { dev, ntfs } = self;
        let dir = ntfs.file(dev, record).ok()?;
        if !dir.is_directory() {
            return None;
        }
        let index = dir.directory_index(dev).ok()?;
        let mut iter = index.entries();
        let mut entries = Vec::new();
        while let Some(entry) = iter.next(dev) {
            let Ok(entry) = entry else { continue };
            let Some(Ok(name)) = entry.key() else { continue };
            // a file with a long name is listed again under its 8.3 alias
            if name.namespace() == NtfsFileNamespace::Dos {
                continue;
            }
            let text = name.name().to_string_lossy();
            // NTFS metadata files ($MFT, $Bitmap, ...) and the root's own
            // "." entry live in the root
            if record == ROOT_RECORD && (text.starts_with('$') || text == ".") {
                continue;
            }
            entries.push(DirEntry {
                name: text,
                stat: UsbNtfsStat {
                    record: entry.file_reference().file_record_number(),
                    // the index's copy of the size can lag; stat for the exact one
                    size: if name.is_directory() { 0 } else { name.data_size() },
                    mtime: name.modification_time().nt_timestamp(),
                    is_dir: name.is_directory(),
                },
            });
        }
        Some(entries)
    }

    fn read_at(&mut self, record: u64, offset: u64, buf: &mut [u8]) -> Option<usize> {
        let Volume { dev, ntfs } = self;
        let file = ntfs.file(dev, record).ok()?;
        let item = file.data(dev, "")?.ok()?;
        let attribute = item.to_attribute().ok()?;
        // the ntfs crate hands back compressed or encrypted data as stored, which
        // would read as garbage: refuse it (sparse runs read as zeros, which is right)
        if attribute.flags().intersects(NtfsAttributeFlags::COMPRESSED | NtfsAttributeFlags::ENCRYPTED) {
            return None;
        }
        let mut value = attribute.value(dev).ok()?;
        if offset >= value.len() {
            return Some(0);
        }
        value.seek(dev, SeekFrom::Start(offset)).ok()?;
        let mut done = 0;
        while done < buf.len() {
            match value.read(dev, &mut buf[done..]) {
                Ok(0) => break,
                Ok(n) => done += n,
                Err(_) => return if done > 0 { Some(done) } else { None },
            }
        }
        Some(done)
    }
}

// ---------------------------------------------------------------------------
// C interface

unsafe fn path_arg<'a>(path: *const c_char) -> Option<&'a str> {
    if path.is_null() {
        return None;
    }
    CStr::from_ptr(path).to_str().ok()
}

/// Mounts the NTFS volume of `size` bytes read through `read`, or null when
/// it is not a readable NTFS volume.
#[no_mangle]
pub unsafe extern "C" fn usbntfs_mount(
    read: ReadSectorsFn,
    user: *mut c_void,
    sector_size: u32,
    size: u64,
) -> *mut Volume {
    if sector_size == 0 || size == 0 {
        return ptr::null_mut();
    }
    let mut dev = Device {
        read,
        user,
        sector_size: sector_size as u64,
        size,
        pos: 0,
        cache: Vec::new(),
    };
    let Ok(mut ntfs) = Ntfs::new(&mut dev) else { return ptr::null_mut() };
    // case-insensitive lookups need the volume's upcase table
    if ntfs.read_upcase_table(&mut dev).is_err() {
        return ptr::null_mut();
    }
    Box::into_raw(Box::new(Volume { dev, ntfs }))
}

#[no_mangle]
pub unsafe extern "C" fn usbntfs_unmount(volume: *mut Volume) {
    if !volume.is_null() {
        drop(Box::from_raw(volume));
    }
}

/// Copies the volume label, NUL-terminated; false when it has none.
#[no_mangle]
pub unsafe extern "C" fn usbntfs_label(volume: *mut Volume, out: *mut c_char, cap: usize) -> bool {
    let Some(volume) = volume.as_mut() else { return false };
    if out.is_null() || cap == 0 {
        return false;
    }
    let Volume { dev, ntfs } = volume;
    let Some(Ok(name)) = ntfs.volume_name(dev) else { return false };
    copy_name(&name.name().to_string_lossy(), out, cap)
}

/// Volume size and cluster size, for statvfs.
#[no_mangle]
pub unsafe extern "C" fn usbntfs_geometry(volume: *mut Volume, size: *mut u64, cluster: *mut u32) {
    if let Some(volume) = volume.as_ref() {
        if !size.is_null() {
            *size = volume.ntfs.size();
        }
        if !cluster.is_null() {
            *cluster = volume.ntfs.cluster_size();
        }
    }
}

/// Looks up a /-separated path from the root; false when it does not exist.
#[no_mangle]
pub unsafe extern "C" fn usbntfs_stat(volume: *mut Volume, path: *const c_char, out: *mut UsbNtfsStat) -> bool {
    let (Some(volume), Some(path)) = (volume.as_mut(), path_arg(path)) else { return false };
    let Some(record) = volume.lookup(path).map(|f| f.file_record_number()) else { return false };
    let Volume { dev, ntfs } = volume;
    let Ok(file) = ntfs.file(dev, record) else { return false };
    if !out.is_null() {
        *out = Volume::stat_of(dev, &file);
    }
    true
}

/// Stat by record number, as returned in UsbNtfsStat::record.
#[no_mangle]
pub unsafe extern "C" fn usbntfs_stat_record(volume: *mut Volume, record: u64, out: *mut UsbNtfsStat) -> bool {
    let Some(volume) = volume.as_mut() else { return false };
    let Volume { dev, ntfs } = volume;
    let Ok(file) = ntfs.file(dev, record) else { return false };
    if !out.is_null() {
        *out = Volume::stat_of(dev, &file);
    }
    true
}

/// Reads up to `len` bytes of a file's data at `offset`. Returns the bytes
/// read (0 at the end), or -1 on error.
#[no_mangle]
pub unsafe extern "C" fn usbntfs_read(volume: *mut Volume, record: u64, offset: u64, buf: *mut u8, len: usize) -> i64 {
    let Some(volume) = volume.as_mut() else { return -1 };
    if buf.is_null() && len > 0 {
        return -1;
    }
    let slice = if len == 0 { &mut [][..] } else { core::slice::from_raw_parts_mut(buf, len) };
    match volume.read_at(record, offset, slice) {
        Some(n) => n as i64,
        None => -1,
    }
}

/// Lists a directory; null when the path is not one.
#[no_mangle]
pub unsafe extern "C" fn usbntfs_opendir(volume: *mut Volume, path: *const c_char) -> *mut Dir {
    let (Some(volume), Some(path)) = (volume.as_mut(), path_arg(path)) else { return ptr::null_mut() };
    match volume.list(path) {
        Some(entries) => Box::into_raw(Box::new(Dir { entries, next: 0 })),
        None => ptr::null_mut(),
    }
}

/// The next entry: its name (UTF-8, NUL-terminated) and stat. False at the end.
#[no_mangle]
pub unsafe extern "C" fn usbntfs_readdir(dir: *mut Dir, name: *mut c_char, cap: usize, out: *mut UsbNtfsStat) -> bool {
    let Some(dir) = dir.as_mut() else { return false };
    while let Some(entry) = dir.entries.get(dir.next) {
        dir.next += 1;
        if copy_name(&entry.name, name, cap) {
            if !out.is_null() {
                *out = entry.stat;
            }
            return true;
        }
    }
    false
}

#[no_mangle]
pub unsafe extern "C" fn usbntfs_rewinddir(dir: *mut Dir) {
    if let Some(dir) = dir.as_mut() {
        dir.next = 0;
    }
}

#[no_mangle]
pub unsafe extern "C" fn usbntfs_closedir(dir: *mut Dir) {
    if !dir.is_null() {
        drop(Box::from_raw(dir));
    }
}

// false when the name does not fit
unsafe fn copy_name(name: &str, out: *mut c_char, cap: usize) -> bool {
    let bytes = name.as_bytes();
    if out.is_null() || bytes.len() + 1 > cap {
        return false;
    }
    ptr::copy_nonoverlapping(bytes.as_ptr(), out as *mut u8, bytes.len());
    *out.add(bytes.len()) = 0;
    true
}

/// Keeps the directory-entry type public for cbindgen-style headers.
#[no_mangle]
pub extern "C" fn usbntfs_version() -> c_int {
    1
}
