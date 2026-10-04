/* Mounts an NTFS image through the sector callback and checks what it reads. */
#define _FILE_OFFSET_BITS 64
#include "usbntfs.h"
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static unsigned long g_reads;

static bool read_sectors(void *user, uint64_t lba, uint32_t count, uint8_t *buf) {
    g_reads++;
    int fd = *(int *)user;
    size_t len = (size_t)count * 512;
    return pread(fd, buf, len, (off_t)(lba * 512)) == (ssize_t)len;
}

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #c); return 1; } } while (0)

int main(int argc, char **argv) {
    int fd = open(argv[1], O_RDONLY);
    CHECK(fd >= 0);
    struct stat st; fstat(fd, &st);
    UsbNtfsVolume *v = usbntfs_mount(read_sectors, &fd, 512, st.st_size - st.st_size % 512);
    CHECK(v);
    char label[64] = "";
    usbntfs_label(v, label, sizeof label);
    printf("label: %s\n", label);

    UsbNtfsDir *d = usbntfs_opendir(v, "/");
    CHECK(d);
    char name[512]; UsbNtfsStat s;
    while (usbntfs_readdir(d, name, sizeof name, &s))
        printf("  %-30s %s %llu\n", name, s.is_dir ? "<DIR>" : "     ", (unsigned long long)s.size);
    usbntfs_closedir(d);

    CHECK(usbntfs_stat(v, "/HELLO.TXT", &s) && !s.is_dir && s.size == 11); /* case-insensitive */
    uint8_t small[32] = {0};
    CHECK(usbntfs_read(v, s.record, 0, small, sizeof small) == 11);
    CHECK(memcmp(small, "hello ntfs\n", 11) == 0);
    CHECK(usbntfs_stat(v, "/ポケモン.gb", &s) && s.size == 12);
    CHECK(!usbntfs_stat(v, "/missing", &s));

    /* the big file, read in odd-sized pieces, against the original */
    CHECK(usbntfs_stat(v, "/big.bin", &s) && s.size == 3000000);
    FILE *ref = fopen(argv[2], "rb");
    CHECK(ref);
    uint8_t *a = malloc(3000000), *b = malloc(3000000);
    CHECK(fread(a, 1, 3000000, ref) == 3000000);
    uint64_t off = 0;
    while (off < s.size) {
        int64_t n = usbntfs_read(v, s.record, off, b + off, 77777);
        CHECK(n > 0);
        off += n;
    }
    CHECK(usbntfs_read(v, s.record, off, b, 10) == 0);
    CHECK(memcmp(a, b, 3000000) == 0);
    if (argc > 3) { /* a nested path, when the image has one */
        CHECK(usbntfs_stat(v, argv[3], &s));
        printf("nested %s: %llu bytes\n", argv[3], (unsigned long long)s.size);
    }
    printf("all checks passed (%lu sector reads)\n", g_reads);
    usbntfs_unmount(v);
    return 0;
}
