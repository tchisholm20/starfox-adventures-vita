// card_shim.c - the GameCube memory card as a directory (vita fork).
//
// Replaces upstream's 2-function stub, whose CARDProbeEx returned READY without
// writing *memSize / *sectorSize: the caller read whatever was on its stack.
//
// Card layout: one directory per card (slot A only; slot B reports NOCARD).
//   <root>/<fileName>        the file's bytes, exactly its created length
//   <root>/<fileName>.stat   the CARDStat bytes, as CARDSetStatus last wrote them
// Root: ux0:data/STFX00001/card/ on the Vita, ./card/ on the host.
//
// Semantics follow the SDK where the game can observe them:
//   CARDCreate: EXIST if present; the new file is zero-filled; its status has
//     iconAddr = commentAddr = 0xFFFFFFFF ("no icon yet") -- the save code branches
//     on exactly that to decide a file is fresh (dlls/engine/2/maketex.c).
//   CARDOpen:   NOFILE if absent.   CARDRead/Write: LIMIT past the file's length.
// NOT emulated: sector alignment rules, async completion (every call is synchronous
// and the *Async forms are not provided), directory/FAT checksums. Save data is
// written in the port's in-memory byte order, so a save is NOT interchangeable
// with a real GameCube card image.
#include <stdio.h>
#include <string.h>
#include "dolphin/types.h"
#include "dolphin/card.h"
#ifdef __vita__
#include <psp2/io/stat.h>
#define CARD_ROOT "ux0:data/STFX00001/card/"
static int card_mkdir(const char* p) { int r = sceIoMkdir(p, 0777); return (r >= 0 || (unsigned)r == 0x80010011u) ? 0 : -1; } // 0x80010011: EEXIST
#else
#include <sys/stat.h>
#define CARD_ROOT "card/"
#ifdef _WIN32
#include <direct.h>
static int card_mkdir(const char* p) { _mkdir(p); return 0; }
#else
static int card_mkdir(const char* p) { mkdir(p, 0777); return 0; }
#endif
#endif

#define CARD_SECTOR 8192
#define CARD_SLOTS  CARD_MAX_FILE

static int  gMounted;
static char gNames[CARD_SLOTS][CARD_FILENAME_MAX + 1];   // fileNo -> name, this session
unsigned card_reads, card_writes, card_errors;           // read by the port's report

static void path(char* out, size_t n, const char* name, const char* ext) {
    snprintf(out, n, "%s%s%s", CARD_ROOT, name, ext);
}
static long file_len(const char* name) {
    char p[160]; FILE* f; long n;
    path(p, sizeof p, name, "");
    if (!(f = fopen(p, "rb"))) return -1;
    fseek(f, 0, SEEK_END); n = ftell(f); fclose(f);
    return n;
}
static int slot_of(const char* name) {   // existing slot, else a new one, else -1
    int i, free_ = -1;
    for (i = 0; i < CARD_SLOTS; ++i) {
        if (gNames[i][0] && strncmp(gNames[i], name, CARD_FILENAME_MAX) == 0) return i;
        if (!gNames[i][0] && free_ < 0) free_ = i;
    }
    if (free_ >= 0) { strncpy(gNames[free_], name, CARD_FILENAME_MAX); gNames[free_][CARD_FILENAME_MAX] = 0; }
    return free_;
}
static int stat_io(const char* name, CARDStat* st, int write) {
    char p[160]; FILE* f; size_t n;
    path(p, sizeof p, name, ".stat");
    if (!(f = fopen(p, write ? "wb" : "rb"))) return -1;
    n = write ? fwrite(st, 1, sizeof *st, f) : fread(st, 1, sizeof *st, f);
    fclose(f);
    return n == sizeof *st ? 0 : -1;
}

void CARDInit(void) { }
s32 CARDProbeEx(s32 chan, s32* memSize, s32* sectorSize) {
    if (chan != 0) return CARD_RESULT_NOCARD;
    if (memSize) *memSize = 4;                 // Mbit: the 59-block card
    if (sectorSize) *sectorSize = CARD_SECTOR;
    return CARD_RESULT_READY;
}
s32 CARDMount(s32 chan, void* workArea, CARDCallback detachCallback) {
    (void)workArea; (void)detachCallback;
    if (chan != 0) return CARD_RESULT_NOCARD;
#ifdef __vita__
    card_mkdir("ux0:data/STFX00001");
#endif
    if (card_mkdir(CARD_ROOT) < 0) { ++card_errors; return CARD_RESULT_NOCARD; }
    gMounted = 1;
    return CARD_RESULT_READY;
}
s32 CARDUnmount(s32 chan) { if (chan == 0) gMounted = 0; return CARD_RESULT_READY; }
s32 CARDCheck(s32 chan) { return (chan == 0 && gMounted) ? CARD_RESULT_READY : CARD_RESULT_NOCARD; }
s32 CARDFormat(s32 chan) {
    int i; char p[160];
    if (chan != 0 || !gMounted) return CARD_RESULT_NOCARD;
    for (i = 0; i < CARD_SLOTS; ++i) if (gNames[i][0]) {
        path(p, sizeof p, gNames[i], ""); remove(p);
        path(p, sizeof p, gNames[i], ".stat"); remove(p);
        gNames[i][0] = 0;
    }
    return CARD_RESULT_READY;
}
s32 CARDGetSerialNo(s32 chan, u64* serialNo) {
    if (chan != 0 || !gMounted) return CARD_RESULT_NOCARD;
    if (serialNo) *serialNo = 0x5354465800000001ull;   // "STFX" + 1: fixed, so the save's identity check holds
    return CARD_RESULT_READY;
}

s32 CARDOpen(s32 chan, const char* fileName, CARDFileInfo* fi) {
    long n; int s;
    if (chan != 0 || !gMounted) return CARD_RESULT_NOCARD;
    if (strlen(fileName) > CARD_FILENAME_MAX) return CARD_RESULT_NAMETOOLONG;
    if ((n = file_len(fileName)) < 0) return CARD_RESULT_NOFILE;
    if ((s = slot_of(fileName)) < 0) return CARD_RESULT_NOENT;
    fi->chan = chan; fi->fileNo = s; fi->offset = 0; fi->length = (s32)n; fi->iBlock = 0;
    return CARD_RESULT_READY;
}
s32 CARDClose(CARDFileInfo* fi) { if (fi) fi->chan = -1; return CARD_RESULT_READY; }

s32 CARDCreate(s32 chan, const char* fileName, u32 size, CARDFileInfo* fi) {
    static const unsigned char zero[512];
    char p[160]; FILE* f; u32 left; CARDStat st; int s;
    if (chan != 0 || !gMounted) return CARD_RESULT_NOCARD;
    if (strlen(fileName) > CARD_FILENAME_MAX) return CARD_RESULT_NAMETOOLONG;
    if (size == 0 || size % CARD_SECTOR) return CARD_RESULT_FATAL_ERROR;   // SDK asserts this
    if (file_len(fileName) >= 0) return CARD_RESULT_EXIST;
    if ((s = slot_of(fileName)) < 0) return CARD_RESULT_NOENT;
    path(p, sizeof p, fileName, "");
    if (!(f = fopen(p, "wb"))) { ++card_errors; return CARD_RESULT_IOERROR; }
    for (left = size; left; ) { u32 k = left < sizeof zero ? left : (u32)sizeof zero; fwrite(zero, 1, k, f); left -= k; }
    fclose(f);
    memset(&st, 0, sizeof st);
    strncpy(st.fileName, fileName, CARD_FILENAME_MAX);
    st.length = size; st.iconAddr = 0xFFFFFFFFu; st.commentAddr = 0xFFFFFFFFu;
    if (stat_io(fileName, &st, 1) < 0) { ++card_errors; return CARD_RESULT_IOERROR; }
    fi->chan = chan; fi->fileNo = s; fi->offset = 0; fi->length = (s32)size; fi->iBlock = 0;
    return CARD_RESULT_READY;
}
s32 CARDDelete(s32 chan, const char* fileName) {
    char p[160]; int i;
    if (chan != 0 || !gMounted) return CARD_RESULT_NOCARD;
    path(p, sizeof p, fileName, "");
    if (remove(p) != 0) return CARD_RESULT_NOFILE;
    path(p, sizeof p, fileName, ".stat"); remove(p);
    for (i = 0; i < CARD_SLOTS; ++i) if (strncmp(gNames[i], fileName, CARD_FILENAME_MAX) == 0) gNames[i][0] = 0;
    return CARD_RESULT_READY;
}

static s32 rw(CARDFileInfo* fi, void* buf, s32 length, s32 offset, int write) {
    char p[160]; FILE* f; size_t n;
    if (!fi || fi->chan != 0 || !gMounted) return CARD_RESULT_NOCARD;
    if ((unsigned)fi->fileNo >= CARD_SLOTS || !gNames[fi->fileNo][0]) return CARD_RESULT_NOFILE;
    if (offset < 0 || length < 0 || offset + length > fi->length) return CARD_RESULT_LIMIT;
    path(p, sizeof p, gNames[fi->fileNo], "");
    if (!(f = fopen(p, write ? "r+b" : "rb"))) { ++card_errors; return CARD_RESULT_IOERROR; }
    fseek(f, offset, SEEK_SET);
    n = write ? fwrite(buf, 1, (size_t)length, f) : fread(buf, 1, (size_t)length, f);
    fclose(f);
    if (n != (size_t)length) { ++card_errors; return CARD_RESULT_IOERROR; }
    if (write) ++card_writes; else ++card_reads;
    return CARD_RESULT_READY;
}
s32 CARDRead(CARDFileInfo* fi, void* buf, s32 length, s32 offset)  { return rw(fi, buf, length, offset, 0); }
s32 CARDWrite(CARDFileInfo* fi, void* buf, s32 length, s32 offset) { return rw(fi, buf, length, offset, 1); }

s32 CARDGetStatus(s32 chan, s32 fileNo, CARDStat* st) {
    if (chan != 0 || !gMounted) return CARD_RESULT_NOCARD;
    if ((unsigned)fileNo >= CARD_SLOTS || !gNames[fileNo][0]) return CARD_RESULT_NOFILE;
    if (stat_io(gNames[fileNo], st, 0) < 0) {           // a file with no sidecar: fresh status
        memset(st, 0, sizeof *st);
        strncpy(st->fileName, gNames[fileNo], CARD_FILENAME_MAX);
        st->length = (u32)file_len(gNames[fileNo]);
        st->iconAddr = st->commentAddr = 0xFFFFFFFFu;
    }
    return CARD_RESULT_READY;
}
s32 CARDSetStatus(s32 chan, s32 fileNo, CARDStat* st) {
    if (chan != 0 || !gMounted) return CARD_RESULT_NOCARD;
    if ((unsigned)fileNo >= CARD_SLOTS || !gNames[fileNo][0]) return CARD_RESULT_NOFILE;
    if (stat_io(gNames[fileNo], st, 1) < 0) { ++card_errors; return CARD_RESULT_IOERROR; }
    return CARD_RESULT_READY;
}
