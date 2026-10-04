#!/usr/bin/env python3
"""Adds read-only NTFS (usbntfs) to libusbhsfs' ISC build.

Run on a libusbhsfs v0.2.9 checkout. Anchored on upstream strings and
idempotent. Everything is guarded by USBNTFS_BUILD, which the Makefile sets
when USBNTFS_INCLUDE (usbntfs.h's directory) is given.
"""
import pathlib, shutil, sys

root = pathlib.Path(sys.argv[1])
here = pathlib.Path(__file__).parent

def edit(rel, pairs):
    path = root / rel
    text = path.read_text()
    if "USBNTFS_BUILD" in text:
        return
    for old, new in pairs:
        if text.count(old) != 1:
            sys.exit(f"{rel}: anchor not found once: {old[:60]!r}")
        text = text.replace(old, new)
    path.write_text(text)

shutil.copytree(here / "source" / "usbntfs", root / "source" / "usbntfs", dirs_exist_ok=True)

edit("Makefile", [(
"""ifeq ($(filter $(MAKECMDGOALS),clean dist-src),)
    # Check BUILD_TYPE flag""",
"""# Read-only NTFS through usbntfs (MIT OR Apache-2.0) for the ISC build.
ifneq ($(USBNTFS_INCLUDE),)
    SOURCES	+=	source/usbntfs
    CFLAGS	+=	-DUSBNTFS_BUILD -I$(USBNTFS_INCLUDE)
endif

ifeq ($(filter $(MAKECMDGOALS),clean dist-src),)
    # Check BUILD_TYPE flag""")])

edit("source/usbhsfs_drive.h", [
("""#ifdef GPL_BUILD
#include "ntfs-3g/ntfs.h"
#include "lwext4/ext.h"
#endif
""",
"""#ifdef GPL_BUILD
#include "ntfs-3g/ntfs.h"
#include "lwext4/ext.h"
#elif defined(USBNTFS_BUILD)
#include "usbntfs/usbntfs_dev.h"
#endif
"""),
("""    ext_vd *ext;        ///< Pointer to a dynamically allocated ext_vd object. Only used if fs_type == UsbHsFsFileSystemType_EXT.
#endif
""",
"""    ext_vd *ext;        ///< Pointer to a dynamically allocated ext_vd object. Only used if fs_type == UsbHsFsFileSystemType_EXT.
#elif defined(USBNTFS_BUILD)
    usbntfs_vd *usbntfs;    ///< Read-only NTFS volume. Only used if fs_type == UsbHsFsFileSystemType_NTFS.
#endif
"""),
("""            case UsbHsFsDriveLogicalUnitFileSystemType_EXT:
                fs_valid = (fs_ctx->ext != NULL);
                break;
#endif
""",
"""            case UsbHsFsDriveLogicalUnitFileSystemType_EXT:
                fs_valid = (fs_ctx->ext != NULL);
                break;
#elif defined(USBNTFS_BUILD)
            case UsbHsFsDriveLogicalUnitFileSystemType_NTFS:
                fs_valid = (fs_ctx->usbntfs != NULL);
                break;
#endif
"""),
])

edit("source/usbhsfs_manager.c", [(
"""        case UsbHsFsDriveLogicalUnitFileSystemType_EXT:
            device->fs_type = fs_ctx->ext->version;
            break;
#endif
""",
"""        case UsbHsFsDriveLogicalUnitFileSystemType_EXT:
            device->fs_type = fs_ctx->ext->version;
            break;
#elif defined(USBNTFS_BUILD)
        case UsbHsFsDriveLogicalUnitFileSystemType_NTFS:
            device->fs_type = UsbHsFsDeviceFileSystemType_NTFS;
            device->write_protect = true;   /* mounted read-only */
            break;
#endif
""")])

edit("source/usbhsfs_mount.c", [
("""static bool usbHsFsMountRegisterExtVolume(UsbHsFsDriveLogicalUnitFileSystemContext *fs_ctx, u64 block_addr, u64 block_count);
static void usbHsFsMountUnregisterExtVolume(UsbHsFsDriveLogicalUnitFileSystemContext *fs_ctx);
#endif
""",
"""static bool usbHsFsMountRegisterExtVolume(UsbHsFsDriveLogicalUnitFileSystemContext *fs_ctx, u64 block_addr, u64 block_count);
static void usbHsFsMountUnregisterExtVolume(UsbHsFsDriveLogicalUnitFileSystemContext *fs_ctx);
#elif defined(USBNTFS_BUILD)
static bool usbHsFsMountRegisterUsbNtfsVolume(UsbHsFsDriveLogicalUnitFileSystemContext *fs_ctx, u64 block_addr, u64 block_count);
static void usbHsFsMountUnregisterUsbNtfsVolume(UsbHsFsDriveLogicalUnitFileSystemContext *fs_ctx);
#endif
"""),
("""#ifndef GPL_BUILD
    NX_IGNORE_ARG(block_count);
#endif
""",
"""#if !defined(GPL_BUILD) && !defined(USBNTFS_BUILD)
    NX_IGNORE_ARG(block_count);
#endif
"""),
("""        case UsbHsFsDriveLogicalUnitFileSystemType_EXT:     /* EXT2/3/4. */
            ret = usbHsFsMountRegisterExtVolume(fs_ctx, block_addr, block_count);
            break;
#endif
""",
"""        case UsbHsFsDriveLogicalUnitFileSystemType_EXT:     /* EXT2/3/4. */
            ret = usbHsFsMountRegisterExtVolume(fs_ctx, block_addr, block_count);
            break;
#elif defined(USBNTFS_BUILD)
        case UsbHsFsDriveLogicalUnitFileSystemType_NTFS:    /* NTFS, read-only. */
            ret = usbHsFsMountRegisterUsbNtfsVolume(fs_ctx, block_addr, block_count);
            break;
#endif
"""),
("""        case UsbHsFsDriveLogicalUnitFileSystemType_EXT:     /* EXT2/3/4. */
            usbHsFsMountUnregisterExtVolume(fs_ctx);
            break;
#endif
""",
"""        case UsbHsFsDriveLogicalUnitFileSystemType_EXT:     /* EXT2/3/4. */
            usbHsFsMountUnregisterExtVolume(fs_ctx);
            break;
#elif defined(USBNTFS_BUILD)
        case UsbHsFsDriveLogicalUnitFileSystemType_NTFS:    /* NTFS, read-only. */
            usbHsFsMountUnregisterUsbNtfsVolume(fs_ctx);
            break;
#endif
"""),
("""        case UsbHsFsDriveLogicalUnitFileSystemType_EXT:     /* EXT2/3/4. */
            fs_device = extdev_get_devoptab();
            break;
#endif
""",
"""        case UsbHsFsDriveLogicalUnitFileSystemType_EXT:     /* EXT2/3/4. */
            fs_device = extdev_get_devoptab();
            break;
#elif defined(USBNTFS_BUILD)
        case UsbHsFsDriveLogicalUnitFileSystemType_NTFS:    /* NTFS, read-only. */
            fs_device = usbntfsdev_get_devoptab();
            break;
#endif
"""),
("""static bool usbHsFsMountRegisterDevoptabDevice(UsbHsFsDriveLogicalUnitFileSystemContext *fs_ctx)
{""",
"""#ifdef USBNTFS_BUILD

/* usbntfs reads the volume's sectors through this: LBAs relative to the volume. */
static bool usbHsFsMountUsbNtfsReadSectors(void *user, uint64_t lba, uint32_t count, uint8_t *buf)
{
    usbntfs_vd *vd = (usbntfs_vd*)user;
    return usbHsFsScsiReadLogicalUnitBlocks((UsbHsFsDriveLogicalUnitContext*)vd->lun_ctx, buf, vd->block_addr + lba, count);
}

static bool usbHsFsMountRegisterUsbNtfsVolume(UsbHsFsDriveLogicalUnitFileSystemContext *fs_ctx, u64 block_addr, u64 block_count)
{
    UsbHsFsDriveLogicalUnitContext *lun_ctx = (UsbHsFsDriveLogicalUnitContext*)fs_ctx->lun_ctx;
    bool ret = false;

    fs_ctx->usbntfs = calloc(1, sizeof(usbntfs_vd));
    if (!fs_ctx->usbntfs)
    {
        USBHSFS_LOG_MSG("Failed to allocate memory for the NTFS volume! (interface %d, LUN %u, FS %u).", lun_ctx->usb_if_id, lun_ctx->lun, fs_ctx->fs_idx);
        goto end;
    }

    fs_ctx->usbntfs->lun_ctx = lun_ctx;
    fs_ctx->usbntfs->block_addr = block_addr;

    /* Mount the volume read-only. Any write through the devoptab fails with EROFS. */
    fs_ctx->usbntfs->volume = usbntfs_mount(usbHsFsMountUsbNtfsReadSectors, fs_ctx->usbntfs, lun_ctx->block_length, block_count * (u64)lun_ctx->block_length);
    if (!fs_ctx->usbntfs->volume)
    {
        USBHSFS_LOG_MSG("Failed to mount NTFS volume! (interface %d, LUN %u, FS %u).", lun_ctx->usb_if_id, lun_ctx->lun, fs_ctx->fs_idx);
        goto end;
    }

    fs_ctx->flags |= UsbHsFsMountFlags_ReadOnly;

    /* Register devoptab device. */
    if (!usbHsFsMountRegisterDevoptabDevice(fs_ctx)) goto end;

    ret = true;

end:
    if (!ret && fs_ctx->usbntfs)
    {
        if (fs_ctx->usbntfs->volume) usbntfs_unmount(fs_ctx->usbntfs->volume);
        free(fs_ctx->usbntfs);
        fs_ctx->usbntfs = NULL;
    }

    return ret;
}

static void usbHsFsMountUnregisterUsbNtfsVolume(UsbHsFsDriveLogicalUnitFileSystemContext *fs_ctx)
{
    if (!fs_ctx->usbntfs) return;
    usbntfs_unmount(fs_ctx->usbntfs->volume);
    free(fs_ctx->usbntfs);
    fs_ctx->usbntfs = NULL;
}

#endif  /* USBNTFS_BUILD */

static bool usbHsFsMountRegisterDevoptabDevice(UsbHsFsDriveLogicalUnitFileSystemContext *fs_ctx)
{"""),
])
print("libusbhsfs patched")
