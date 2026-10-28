// dvd_smoketest.c - validate the host DVD/file layer against a synthetic disc.
//
// Creates a small "extracted disc" (a dir with two files + filetable.txt), then
// exercises DVDOpen/Read, loadFileByPath, fileLoad(id) caching, offset reads, EOF
// padding, and async callback. The file layer is game-data-agnostic, so real bytes
// (any bytes) prove correctness; point dvd_shim at the real orig/GSAE01 to load
// actual assets.

#include "port/dvd_shim.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <direct.h>
static void makedir(const char* p) { _mkdir(p); }
#else
#include <sys/stat.h>
static void makedir(const char* p) { mkdir(p, 0755); }
#endif

static int gPass = 0, gFail = 0;
static void check(const char* what, int ok) {
    printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (ok) ++gPass; else ++gFail;
}

static void writeFile(const char* path, const void* data, int size) {
    FILE* f = fopen(path, "wb");
    if (f) { fwrite(data, 1, (size_t)size, f); fclose(f); }
}

static int gAsyncResult = -999;
static void asyncCb(int32_t result, DVDFileInfo* info) { (void)info; gAsyncResult = result; }

int main(int argc, char** argv) {
    const char* root = (argc > 1) ? argv[1] : "dvd_fixture";

    // --- build the fixture disc ---
    char dir[512], path[512];
    makedir(root);
    snprintf(dir, sizeof(dir), "%s/test", root); makedir(dir);
    snprintf(dir, sizeof(dir), "%s/data", root); makedir(dir);

    unsigned char hello[32];
    for (int i = 0; i < 32; ++i) hello[i] = (unsigned char)i;   // 0x00..0x1F
    snprintf(path, sizeof(path), "%s/test/hello.bin", root);
    writeFile(path, hello, sizeof(hello));

    const char* second = "SECOND-FILE-CONTENTS-0123456789";  // 31 bytes + NUL region
    int secondLen = (int)strlen(second);
    snprintf(path, sizeof(path), "%s/data/second.bin", root);
    writeFile(path, second, secondLen);

    snprintf(path, sizeof(path), "%s/filetable.txt", root);
    {
        FILE* f = fopen(path, "w");
        fprintf(f, "# id  relpath\n0 test/hello.bin\n1 data/second.bin\n");
        fclose(f);
    }

    // --- exercise the layer ---
    printf("[dvd_smoketest] root=%s\n", root);
    dvd_shim_init(root);

    // 1) loadFileByPath
    int sz = -1;
    unsigned char* p = (unsigned char*)loadFileByPath((char*)"test/hello.bin", &sz, 0);
    check("loadFileByPath returns buffer", p != NULL);
    check("loadFileByPath size == 32", sz == 32);
    check("loadFileByPath bytes[0]==0 bytes[31]==31", p && p[0] == 0 && p[31] == 31);

    // 2) DVDOpen + offset read
    DVDFileInfo info;
    int opened = DVDOpen("test/hello.bin", &info);
    check("DVDOpen succeeds", opened != 0);
    check("DVDFileInfo.length == 32", info.length == 32);
    unsigned char four[4] = {0};
    int rd = DVDRead(&info, four, 4, 8);   // bytes at offset 8 -> 8,9,10,11
    check("DVDRead offset=8 returns 4", rd == 4);
    check("DVDRead offset bytes == {8,9,10,11}", four[0]==8 && four[1]==9 && four[2]==10 && four[3]==11);

    // 3) EOF padding: read 48 from a 32-byte file
    unsigned char big[48];
    memset(big, 0xAA, sizeof(big));
    int rd2 = DVDRead(&info, big, 48, 0);
    check("DVDRead past EOF returns requested size", rd2 == 48);
    check("DVDRead pads past EOF with zero", big[32] == 0 && big[47] == 0);
    DVDClose(&info);

    // 4) fileLoad(id) + cache identity
    void* a = fileLoad(0, 0);
    void* b = fileLoad(0, 0);
    check("fileLoad(0) returns buffer", a != NULL);
    check("fileLoad(0) cached: same pointer on repeat", a == b);
    check("fileLoad(0) bytes correct", a && ((unsigned char*)a)[10] == 10);

    // 5) fileLoadToBufferOffset on id 1
    char dst[8] = {0};
    int n = fileLoadToBufferOffset(1, dst, 7, 6);  // "FILE-C" from "SECOND-FILE-..."
    check("fileLoadToBufferOffset returns size", n == 6);
    check("fileLoadToBufferOffset bytes", memcmp(dst, second + 7, 6) == 0);

    // 6) async read fires callback
    unsigned char abuf[32];
    DVDFileInfo ai;
    DVDOpen("test/hello.bin", &ai);
    DVDReadAsyncPrio(&ai, abuf, 32, 0, asyncCb, 2);
    DVDClose(&ai);
    check("async callback fired with result==32", gAsyncResult == 32);

    // 7) missing mapping
    check("fileLoad(99) with no mapping returns NULL", fileLoad(99, 0) == NULL);

    free(p);
    dvd_shim_shutdown();

    printf("[dvd_smoketest] %d passed, %d failed\n", gPass, gFail);
    return gFail ? 1 : 0;
}
