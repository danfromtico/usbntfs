/*
 * usbntfs_dev.c
 *
 * Read-only NTFS for libusbhsfs' ISC build: a devoptab over usbntfs
 * (MIT OR Apache-2.0). Follows the conventions of ff_dev.c. ISC licensed,
 * like libusbhsfs.
 */

#include <sys/param.h>
#include <fcntl.h>

#include "../usbhsfs_manager.h"
#include "../usbhsfs_mount.h"

/* Helper macros, as in ff_dev.c. */

#define un_end                      goto end
#define un_ended_with_error         (_errno != 0)
#define un_set_error(x)             r->_errno = _errno = (x)
#define un_set_error_and_exit(x)    \
do { \
    un_set_error((x)); \
    un_end; \
} while(0)

#define un_declare_error_state      int _errno = 0
#define un_declare_fs_ctx           UsbHsFsDriveLogicalUnitFileSystemContext *fs_ctx = (UsbHsFsDriveLogicalUnitFileSystemContext*)r->deviceData
#define un_declare_lun_ctx          UsbHsFsDriveLogicalUnitContext *lun_ctx = (UsbHsFsDriveLogicalUnitContext*)fs_ctx->lun_ctx
#define un_declare_drive_ctx        UsbHsFsDriveContext *drive_ctx = (UsbHsFsDriveContext*)lun_ctx->drive_ctx

#define un_lock_drive_ctx           un_declare_fs_ctx; \
                                    un_declare_lun_ctx; \
                                    un_declare_drive_ctx; \
                                    bool drive_ctx_valid = usbHsFsManagerIsDriveContextPointerValid(drive_ctx); \
                                    if (!drive_ctx_valid) un_set_error_and_exit(ENODEV)

#define un_unlock_drive_ctx         if (drive_ctx_valid) mutexUnlock(&(drive_ctx->mutex))

#define un_return(x)                return (un_ended_with_error ? -1 : (x))

/* Seconds between 1601-01-01 (NT time) and 1970-01-01 (Unix time). */
#define NT_TO_UNIX_EPOCH            11644473600ULL

typedef struct {
    u64 record;
    u64 size;
    u64 pos;
} usbntfs_file;

typedef struct {
    UsbNtfsDir *dir;
} usbntfs_dir;

/* Function prototypes. */

static int       usbntfsdev_open(struct _reent *r, void *fd, const char *path, int flags, int mode);
static int       usbntfsdev_close(struct _reent *r, void *fd);
static ssize_t   usbntfsdev_write(struct _reent *r, void *fd, const char *ptr, size_t len);
static ssize_t   usbntfsdev_read(struct _reent *r, void *fd, char *ptr, size_t len);
static off_t     usbntfsdev_seek(struct _reent *r, void *fd, off_t pos, int dir);
static int       usbntfsdev_fstat(struct _reent *r, void *fd, struct stat *st);
static int       usbntfsdev_stat(struct _reent *r, const char *file, struct stat *st);
static int       usbntfsdev_link(struct _reent *r, const char *existing, const char *newLink);
static int       usbntfsdev_unlink(struct _reent *r, const char *name);
static int       usbntfsdev_chdir(struct _reent *r, const char *name);
static int       usbntfsdev_rename(struct _reent *r, const char *oldName, const char *newName);
static int       usbntfsdev_mkdir(struct _reent *r, const char *path, int mode);
static DIR_ITER* usbntfsdev_diropen(struct _reent *r, DIR_ITER *dirState, const char *path);
static int       usbntfsdev_dirreset(struct _reent *r, DIR_ITER *dirState);
static int       usbntfsdev_dirnext(struct _reent *r, DIR_ITER *dirState, char *filename, struct stat *filestat);
static int       usbntfsdev_dirclose(struct _reent *r, DIR_ITER *dirState);
static int       usbntfsdev_statvfs(struct _reent *r, const char *path, struct statvfs *buf);
static int       usbntfsdev_ftruncate(struct _reent *r, void *fd, off_t len);
static int       usbntfsdev_fsync(struct _reent *r, void *fd);
static int       usbntfsdev_chmod(struct _reent *r, const char *path, mode_t mode);
static int       usbntfsdev_fchmod(struct _reent *r, void *fd, mode_t mode);
static int       usbntfsdev_rmdir(struct _reent *r, const char *name);
static int       usbntfsdev_utimes(struct _reent *r, const char *filename, const struct timeval times[2]);

static bool usbntfsdev_fixpath(struct _reent *r, const char *path, UsbHsFsDriveLogicalUnitFileSystemContext *fs_ctx);
static void usbntfsdev_fill_stat(UsbHsFsDriveLogicalUnitFileSystemContext *fs_ctx, struct stat *st, const UsbNtfsStat *info);

/* Global variables. */

static const devoptab_t usbntfsdev_devoptab = {
    .name         = NULL,
    .structSize   = sizeof(usbntfs_file),
    .open_r       = usbntfsdev_open,
    .close_r      = usbntfsdev_close,
    .write_r      = usbntfsdev_write,
    .read_r       = usbntfsdev_read,
    .seek_r       = usbntfsdev_seek,
    .fstat_r      = usbntfsdev_fstat,
    .stat_r       = usbntfsdev_stat,
    .link_r       = usbntfsdev_link,
    .unlink_r     = usbntfsdev_unlink,
    .chdir_r      = usbntfsdev_chdir,
    .rename_r     = usbntfsdev_rename,
    .mkdir_r      = usbntfsdev_mkdir,
    .dirStateSize = sizeof(usbntfs_dir),
    .diropen_r    = usbntfsdev_diropen,
    .dirreset_r   = usbntfsdev_dirreset,
    .dirnext_r    = usbntfsdev_dirnext,
    .dirclose_r   = usbntfsdev_dirclose,
    .statvfs_r    = usbntfsdev_statvfs,
    .ftruncate_r  = usbntfsdev_ftruncate,
    .fsync_r      = usbntfsdev_fsync,
    .deviceData   = NULL,
    .chmod_r      = usbntfsdev_chmod,
    .fchmod_r     = usbntfsdev_fchmod,
    .rmdir_r      = usbntfsdev_rmdir,
    .lstat_r      = usbntfsdev_stat,    ///< Symlinks aren't followed, so lstat() is stat().
    .utimes_r     = usbntfsdev_utimes
};

const devoptab_t *usbntfsdev_get_devoptab()
{
    return &usbntfsdev_devoptab;
}

static int usbntfsdev_open(struct _reent *r, void *fd, const char *path, int flags, int mode)
{
    NX_IGNORE_ARG(mode);

    usbntfs_file *file = (usbntfs_file*)fd;
    UsbNtfsStat info = {0};

    un_declare_error_state;
    un_lock_drive_ctx;

    if (!file) un_set_error_and_exit(EINVAL);

    /* Read-only filesystem. */
    if ((flags & O_ACCMODE) != O_RDONLY || (flags & (O_CREAT | O_TRUNC | O_APPEND))) un_set_error_and_exit(EROFS);

    if (!usbntfsdev_fixpath(r, path, fs_ctx)) un_end;

    USBHSFS_LOG_MSG("Opening file \"%s\" (\"%s\") with flags 0x%X.", path, __usbhsfs_dev_path_buf, flags);

    if (!usbntfs_stat(fs_ctx->usbntfs->volume, __usbhsfs_dev_path_buf, &info)) un_set_error_and_exit(ENOENT);
    if (info.is_dir) un_set_error_and_exit(EISDIR);

    file->record = info.record;
    file->size = info.size;
    file->pos = 0;

end:
    un_unlock_drive_ctx;
    un_return(0);
}

static int usbntfsdev_close(struct _reent *r, void *fd)
{
    un_declare_error_state;

    if (!fd) un_set_error_and_exit(EINVAL);
    memset(fd, 0, sizeof(usbntfs_file));

end:
    un_return(0);
}

static ssize_t usbntfsdev_write(struct _reent *r, void *fd, const char *ptr, size_t len)
{
    NX_IGNORE_ARG(fd);
    NX_IGNORE_ARG(ptr);
    NX_IGNORE_ARG(len);
    r->_errno = EROFS;
    return -1;
}

static ssize_t usbntfsdev_read(struct _reent *r, void *fd, char *ptr, size_t len)
{
    usbntfs_file *file = (usbntfs_file*)fd;
    s64 res = 0;

    un_declare_error_state;
    un_lock_drive_ctx;

    if (!file || !ptr || !len) un_set_error_and_exit(EINVAL);

    if (file->pos >= file->size) un_end; /* EOF */

    res = usbntfs_read(fs_ctx->usbntfs->volume, file->record, file->pos, (u8*)ptr, len);
    if (res < 0) un_set_error_and_exit(EIO);

    file->pos += (u64)res;

end:
    un_unlock_drive_ctx;
    un_return((ssize_t)res);
}

static off_t usbntfsdev_seek(struct _reent *r, void *fd, off_t pos, int dir)
{
    usbntfs_file *file = (usbntfs_file*)fd;
    s64 offset = 0;

    un_declare_error_state;

    if (!file) un_set_error_and_exit(EINVAL);

    switch(dir)
    {
        case SEEK_SET:
            break;
        case SEEK_CUR:
            offset = (s64)file->pos;
            break;
        case SEEK_END:
            offset = (s64)file->size;
            break;
        default:
            un_set_error_and_exit(EINVAL);
    }

    if (pos < 0 && offset < -pos) un_set_error_and_exit(EINVAL);
    file->pos = (u64)(offset + pos);

end:
    un_return((off_t)(file ? file->pos : 0));
}

static int usbntfsdev_fstat(struct _reent *r, void *fd, struct stat *st)
{
    usbntfs_file *file = (usbntfs_file*)fd;
    UsbNtfsStat info = {0};

    un_declare_error_state;
    un_lock_drive_ctx;

    if (!file || !st) un_set_error_and_exit(EINVAL);

    if (!usbntfs_stat_record(fs_ctx->usbntfs->volume, file->record, &info)) un_set_error_and_exit(EIO);
    usbntfsdev_fill_stat(fs_ctx, st, &info);

end:
    un_unlock_drive_ctx;
    un_return(0);
}

static int usbntfsdev_stat(struct _reent *r, const char *file, struct stat *st)
{
    UsbNtfsStat info = {0};

    un_declare_error_state;
    un_lock_drive_ctx;

    if (!st) un_set_error_and_exit(EINVAL);

    if (!usbntfsdev_fixpath(r, file, fs_ctx)) un_end;

    USBHSFS_LOG_MSG("Getting stats for \"%s\" (\"%s\").", file, __usbhsfs_dev_path_buf);

    if (!usbntfs_stat(fs_ctx->usbntfs->volume, __usbhsfs_dev_path_buf, &info)) un_set_error_and_exit(ENOENT);
    usbntfsdev_fill_stat(fs_ctx, st, &info);

end:
    un_unlock_drive_ctx;
    un_return(0);
}

static int usbntfsdev_link(struct _reent *r, const char *existing, const char *newLink)
{
    NX_IGNORE_ARG(existing);
    NX_IGNORE_ARG(newLink);
    r->_errno = EROFS;
    return -1;
}

static int usbntfsdev_unlink(struct _reent *r, const char *name)
{
    NX_IGNORE_ARG(name);
    r->_errno = EROFS;
    return -1;
}

static int usbntfsdev_chdir(struct _reent *r, const char *name)
{
    UsbNtfsStat info = {0};
    size_t cwd_len = 0;

    un_declare_error_state;
    un_lock_drive_ctx;

    if (!usbntfsdev_fixpath(r, name, fs_ctx)) un_end;

    USBHSFS_LOG_MSG("Changing current directory to \"%s\" (\"%s\").", name, __usbhsfs_dev_path_buf);

    if (!usbntfs_stat(fs_ctx->usbntfs->volume, __usbhsfs_dev_path_buf, &info)) un_set_error_and_exit(ENOENT);
    if (!info.is_dir) un_set_error_and_exit(ENOTDIR);

    /* Update current working directory. */
    snprintf(fs_ctx->cwd, MAX_PATH_LENGTH, "%s", __usbhsfs_dev_path_buf);

    cwd_len = strlen(fs_ctx->cwd);
    if (cwd_len + 1 < MAX_PATH_LENGTH && fs_ctx->cwd[cwd_len - 1] != '/')
    {
        fs_ctx->cwd[cwd_len] = '/';
        fs_ctx->cwd[cwd_len + 1] = '\0';
    }

    /* Set default devoptab device. */
    usbHsFsMountSetDefaultDevoptabDevice(fs_ctx);

end:
    un_unlock_drive_ctx;
    un_return(0);
}

static int usbntfsdev_rename(struct _reent *r, const char *oldName, const char *newName)
{
    NX_IGNORE_ARG(oldName);
    NX_IGNORE_ARG(newName);
    r->_errno = EROFS;
    return -1;
}

static int usbntfsdev_mkdir(struct _reent *r, const char *path, int mode)
{
    NX_IGNORE_ARG(path);
    NX_IGNORE_ARG(mode);
    r->_errno = EROFS;
    return -1;
}

static DIR_ITER *usbntfsdev_diropen(struct _reent *r, DIR_ITER *dirState, const char *path)
{
    DIR_ITER *ret = NULL;

    un_declare_error_state;
    un_lock_drive_ctx;

    if (!dirState) un_set_error_and_exit(EINVAL);

    usbntfs_dir *dir = (usbntfs_dir*)dirState->dirStruct;

    if (!usbntfsdev_fixpath(r, path, fs_ctx)) un_end;

    USBHSFS_LOG_MSG("Opening directory \"%s\" (\"%s\").", path, __usbhsfs_dev_path_buf);

    dir->dir = usbntfs_opendir(fs_ctx->usbntfs->volume, __usbhsfs_dev_path_buf);
    if (!dir->dir) un_set_error_and_exit(ENOENT);

    ret = dirState;

end:
    un_unlock_drive_ctx;
    return (un_ended_with_error ? NULL : ret);
}

static int usbntfsdev_dirreset(struct _reent *r, DIR_ITER *dirState)
{
    un_declare_error_state;

    if (!dirState) un_set_error_and_exit(EINVAL);
    usbntfs_rewinddir(((usbntfs_dir*)dirState->dirStruct)->dir);

end:
    un_return(0);
}

static int usbntfsdev_dirnext(struct _reent *r, DIR_ITER *dirState, char *filename, struct stat *filestat)
{
    UsbNtfsStat info = {0};

    un_declare_error_state;
    un_lock_drive_ctx;

    if (!dirState || !filename || !filestat) un_set_error_and_exit(EINVAL);

    /* newlib's filename buffer holds NAME_MAX + 1 bytes. ENOENT signals the end. */
    if (!usbntfs_readdir(((usbntfs_dir*)dirState->dirStruct)->dir, filename, NAME_MAX + 1, &info)) un_set_error_and_exit(ENOENT);

    usbntfsdev_fill_stat(fs_ctx, filestat, &info);

end:
    un_unlock_drive_ctx;
    un_return(0);
}

static int usbntfsdev_dirclose(struct _reent *r, DIR_ITER *dirState)
{
    un_declare_error_state;

    if (!dirState) un_set_error_and_exit(EINVAL);

    usbntfs_dir *dir = (usbntfs_dir*)dirState->dirStruct;
    usbntfs_closedir(dir->dir);
    dir->dir = NULL;

end:
    un_return(0);
}

static int usbntfsdev_statvfs(struct _reent *r, const char *path, struct statvfs *buf)
{
    NX_IGNORE_ARG(path);

    u64 size = 0;
    u32 cluster = 0;

    un_declare_error_state;
    un_lock_drive_ctx;

    if (!buf) un_set_error_and_exit(EINVAL);

    usbntfs_geometry(fs_ctx->usbntfs->volume, &size, &cluster);
    if (!cluster) cluster = 4096;

    memset(buf, 0, sizeof(struct statvfs));
    buf->f_bsize = cluster;
    buf->f_frsize = cluster;
    buf->f_blocks = size / cluster;
    buf->f_bfree = 0;   /* read-only: nothing can be written */
    buf->f_bavail = 0;
    buf->f_fsid = fs_ctx->device_id;
    buf->f_flag = (ST_NOSUID | ST_RDONLY);
    buf->f_namemax = NAME_MAX;

end:
    un_unlock_drive_ctx;
    un_return(0);
}

static int usbntfsdev_ftruncate(struct _reent *r, void *fd, off_t len)
{
    NX_IGNORE_ARG(fd);
    NX_IGNORE_ARG(len);
    r->_errno = EROFS;
    return -1;
}

static int usbntfsdev_fsync(struct _reent *r, void *fd)
{
    NX_IGNORE_ARG(r);
    NX_IGNORE_ARG(fd);
    return 0;   /* nothing is ever written */
}

static int usbntfsdev_chmod(struct _reent *r, const char *path, mode_t mode)
{
    NX_IGNORE_ARG(path);
    NX_IGNORE_ARG(mode);
    r->_errno = EROFS;
    return -1;
}

static int usbntfsdev_fchmod(struct _reent *r, void *fd, mode_t mode)
{
    NX_IGNORE_ARG(fd);
    NX_IGNORE_ARG(mode);
    r->_errno = EROFS;
    return -1;
}

static int usbntfsdev_rmdir(struct _reent *r, const char *name)
{
    NX_IGNORE_ARG(name);
    r->_errno = EROFS;
    return -1;
}

static int usbntfsdev_utimes(struct _reent *r, const char *filename, const struct timeval times[2])
{
    NX_IGNORE_ARG(filename);
    NX_IGNORE_ARG(times);
    r->_errno = EROFS;
    return -1;
}

/* Turns a devoptab path ("ums0:/a/../b", or one relative to the current
 * directory) into an absolute /-separated path in __usbhsfs_dev_path_buf. */
static bool usbntfsdev_fixpath(struct _reent *r, const char *path, UsbHsFsDriveLogicalUnitFileSystemContext *fs_ctx)
{
    char joined[MAX_PATH_LENGTH * 2] = {0};
    char *out = __usbhsfs_dev_path_buf;
    size_t len = 0;

    un_declare_error_state;

    if (!r || !path || !*path || !fs_ctx || !fs_ctx->cwd) un_set_error_and_exit(EINVAL);

    /* Strip the mount name. */
    const char *colon = strchr(path, ':');
    if (colon) path = colon + 1;

    if (*path == '/')
        snprintf(joined, sizeof(joined), "%s", path);
    else
        snprintf(joined, sizeof(joined), "%s%s", fs_ctx->cwd, path);

    /* Rebuild component by component, resolving "." and "..". */
    out[0] = '/';
    len = 1;
    for (char *save = NULL, *part = strtok_r(joined, "/", &save); part; part = strtok_r(NULL, "/", &save))
    {
        if (!strcmp(part, ".")) continue;
        if (!strcmp(part, ".."))
        {
            while (len > 1 && out[len - 1] != '/') len--;   /* drop the last component */
            if (len > 1) len--;                             /* and its slash */
            continue;
        }

        size_t part_len = strlen(part);
        if (len + (len > 1 ? 1 : 0) + part_len >= MAX_PATH_LENGTH) un_set_error_and_exit(ENAMETOOLONG);
        if (len > 1) out[len++] = '/';
        memcpy(out + len, part, part_len);
        len += part_len;
    }
    out[len] = '\0';

end:
    return !un_ended_with_error;
}

static void usbntfsdev_fill_stat(UsbHsFsDriveLogicalUnitFileSystemContext *fs_ctx, struct stat *st, const UsbNtfsStat *info)
{
    u32 cluster = 0;
    usbntfs_geometry(fs_ctx->usbntfs->volume, NULL, &cluster);

    memset(st, 0, sizeof(struct stat));

    st->st_ino = info->record;
    st->st_nlink = 1;
    st->st_size = (off_t)info->size;
    st->st_blksize = cluster ? cluster : 4096;
    st->st_blocks = (info->size + 511) / 512;
    st->st_mode = info->is_dir ? (S_IFDIR | S_IRWXU | S_IRWXG | S_IRWXO) : (S_IFREG | S_IRUSR | S_IRGRP | S_IROTH);

    time_t t = (info->mtime / 10000000ULL > NT_TO_UNIX_EPOCH) ? (time_t)(info->mtime / 10000000ULL - NT_TO_UNIX_EPOCH) : 0;
    st->st_atime = st->st_mtime = st->st_ctime = t;
}
