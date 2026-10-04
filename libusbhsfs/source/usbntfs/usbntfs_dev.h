/*
 * usbntfs_dev.h
 *
 * Read-only NTFS for libusbhsfs' ISC build, through usbntfs (MIT OR Apache-2.0).
 * This file is ISC licensed, like libusbhsfs.
 */

#pragma once

#ifndef __USBNTFS_DEV_H__
#define __USBNTFS_DEV_H__

#include <usbntfs.h>

/// A mounted volume: the usbntfs handle and where its sectors are read from.
typedef struct {
    UsbNtfsVolume *volume;
    void *lun_ctx;      ///< UsbHsFsDriveLogicalUnitContext.
    u64 block_addr;     ///< First LBA of the volume on the LUN.
} usbntfs_vd;

/// Returns a pointer to the devoptab interface for read-only NTFS volumes.
const devoptab_t *usbntfsdev_get_devoptab();

#endif  /* __USBNTFS_DEV_H__ */
