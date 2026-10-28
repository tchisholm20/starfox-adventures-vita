// disc_ls.c - mount a GameCube ISO through dvd_shim and list its files.
//
//   disc_ls <iso-or-dir> [maxList]
//
// Proves the DVD layer reads a real disc: prints the game code and FST entries,
// then reads the first bytes of a real file straight out of the image.

#include "port/dvd_shim.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char** argv) {
    if (argc < 2) { fprintf(stderr, "usage: disc_ls <iso-or-dir> [maxList]\n"); return 2; }
    const char* root = argv[1];
    int maxList = (argc > 2) ? atoi(argv[2]) : 30;

    dvd_shim_init(root);
    if (!dvd_shim_isIso()) {
        printf("[disc_ls] '%s' is not a mounted ISO (directory mode)\n", root);
        return 0;
    }

    int count = dvd_shim_fileCount();
    printf("[disc_ls] game=%s files=%d\n", dvd_shim_gameCode(), count);
    printf("[disc_ls] first %d entries:\n", maxList < count ? maxList : count);

    long long total = 0;
    const char* firstReadable = NULL;
    int firstReadableSize = 0;
    for (int i = 0; i < count; ++i) {
        int sz = 0;
        const char* name = dvd_shim_fileName(i, &sz);
        total += (unsigned)sz;
        if (i < maxList) printf("  %6d  %10d  %s\n", i, sz, name);
        if (!firstReadable && sz > 16) { firstReadable = name; firstReadableSize = sz; }
    }
    printf("[disc_ls] total file bytes: %lld (%.1f MB)\n", total, total / (1024.0 * 1024.0));

    // Read the first 16 bytes of a real file, straight from the image.
    if (firstReadable) {
        int sz = 0;
        unsigned char* data = (unsigned char*)loadFileByPath((char*)firstReadable, &sz, 0);
        if (data) {
            printf("[disc_ls] read '%s' (%d bytes). first 16: ", firstReadable, sz);
            for (int i = 0; i < 16 && i < sz; ++i) printf("%02X ", data[i]);
            printf("\n");
            free(data);
        } else {
            printf("[disc_ls] FAILED to read '%s'\n", firstReadable);
        }
        (void)firstReadableSize;
    }

    dvd_shim_shutdown();
    return 0;
}
