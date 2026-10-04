/* Walks a populated NTFS image: big directories, nesting, Unicode, fragmentation. */
#define _FILE_OFFSET_BITS 64
#include "usbntfs.h"
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static bool read_sectors(void *user, uint64_t lba, uint32_t count, uint8_t *buf) {
    int fd = *(int *)user;
    size_t len = (size_t)count * 512;
    return pread(fd, buf, len, (off_t)(lba * 512)) == (ssize_t)len;
}

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #c); exit(1); } } while (0)

static int count_entries(UsbNtfsVolume *v, const char *path) {
    UsbNtfsDir *d = usbntfs_opendir(v, path);
    CHECK(d);
    char name[512]; UsbNtfsStat s; int n = 0;
    while (usbntfs_readdir(d, name, sizeof name, &s)) n++;
    usbntfs_closedir(d);
    return n;
}

static void walk(UsbNtfsVolume *v, const char *path, int depth, int *files) {
    UsbNtfsDir *d = usbntfs_opendir(v, path);
    CHECK(d);
    char name[512]; UsbNtfsStat s;
    while (usbntfs_readdir(d, name, sizeof name, &s)) {
        char child[1024];
        snprintf(child, sizeof child, "%s/%s", strcmp(path, "/") ? path : "", name);
        if (s.is_dir) {
            printf("%*s%s/\n", depth * 2, "", name);
            walk(v, child, depth + 1, files);
        } else {
            ++*files;
            if (depth < 3 && *files % 100 == 1) printf("%*s%s (%llu)\n", depth * 2, "", name, (unsigned long long)s.size);
        }
    }
    usbntfs_closedir(d);
}

static void compare(UsbNtfsVolume *v, const char *path, const char *ref_path) {
    UsbNtfsStat s;
    CHECK(usbntfs_stat(v, path, &s));
    FILE *ref = fopen(ref_path, "rb"); CHECK(ref);
    fseek(ref, 0, SEEK_END); long size = ftell(ref); rewind(ref);
    CHECK((long)s.size == size);
    uint8_t *a = malloc(size), *b = malloc(size);
    CHECK(fread(a, 1, size, ref) == (size_t)size);
    for (uint64_t off = 0; off < s.size;) {
        int64_t n = usbntfs_read(v, s.record, off, b + off, 100000);
        CHECK(n > 0); off += n;
    }
    CHECK(memcmp(a, b, size) == 0);
    printf("ok: %s matches %s (%ld bytes)\n", path, ref_path, size);
    free(a); free(b); fclose(ref);
}

int main(int argc, char **argv) {
    int fd = open(argv[1], O_RDONLY); CHECK(fd >= 0);
    struct stat st; fstat(fd, &st);
    UsbNtfsVolume *v = usbntfs_mount(read_sectors, &fd, 512, st.st_size);
    CHECK(v);
    int files = 0;
    walk(v, "/", 0, &files);
    printf("files: %d\n", files);
    CHECK(count_entries(v, "/roms/gb") == 600);
    UsbNtfsStat s;
    CHECK(usbntfs_stat(v, "/roms/gb/game NUMBER 599 (usa).GB", &s) && s.size == 8);
    CHECK(usbntfs_stat(v, "/Jogos Ação/日本/ファイル.txt", &s) && s.size == 5);
    CHECK(usbntfs_stat(v, "/roms", &s) && s.is_dir);
    CHECK(!usbntfs_opendir(v, "/roms/gb/Game number 1 (USA).gb"));
    compare(v, "/roms/snes/RPG/Chrono Trigger (USA).sfc", argv[2]);
    compare(v, "/frag_a.bin", argv[3]);
    usbntfs_unmount(v);
    puts("tree checks passed");
    return 0;
}
