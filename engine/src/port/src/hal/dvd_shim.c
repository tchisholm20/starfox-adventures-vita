// dvd_shim.c - DVD / fileio backed by the host filesystem or a GameCube ISO.
//
// Two disc-root modes, auto-detected in dvd_shim_init:
//   - a directory: files read as <root>/<path>.
//   - an ISO/GCM image: the GameCube disc header + FST (file string table) are
//     parsed so a path resolves to a (byte offset, size) inside the image and
//     DVDRead reads straight from the image. Disc data is big-endian; the FST is
//     big-endian too. Bytes are returned unswapped; consumers byte-swap.
//
// fileLoad(id) still maps id -> name (filetable.txt) -> DVDOpen/Read, cached by id
// so a repeat fileLoad returns the same pointer (the game relies on this).

#include "port/dvd_shim.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DVD_MAX_FILE_ID 2048

// --- disc root -------------------------------------------------------------
static char  gDiscRoot[512] = ".";
static int   gAutoInvalidate = 0;

// --- id -> name table + cache (fileLoad) -----------------------------------
static char* gNameTable[DVD_MAX_FILE_ID];
static void* gCache[DVD_MAX_FILE_ID];
static int   gCacheSize[DVD_MAX_FILE_ID];

// --- ISO mount state -------------------------------------------------------
typedef struct IsoEntry { char* path; unsigned offset; unsigned size; } IsoEntry;
static struct {
    int       mounted;
    FILE*     fp;
    char      gameCode[8];
    IsoEntry* entries;
    int       count, cap;
} gIso;

static unsigned be32(const unsigned char* p) {
    return ((unsigned)p[0] << 24) | ((unsigned)p[1] << 16) | ((unsigned)p[2] << 8) | p[3];
}
static unsigned be24(const unsigned char* p) {
    return ((unsigned)p[0] << 16) | ((unsigned)p[1] << 8) | p[2];
}

static int ieq(char a, char b) {
    if (a >= 'A' && a <= 'Z') a += 32;
    if (b >= 'A' && b <= 'Z') b += 32;
    return a == b;
}
static int pathEqual(const char* a, const char* b) { // case-insensitive, '\'=='/'
    while (*a || *b) {
        char ca = (*a == '\\') ? '/' : *a;
        char cb = (*b == '\\') ? '/' : *b;
        if (!ieq(ca, cb)) return 0;
        ++a; ++b;
    }
    return 1;
}

static void isoAddEntry(const char* path, unsigned off, unsigned size) {
    if (gIso.count == gIso.cap) {
        gIso.cap = gIso.cap ? gIso.cap * 2 : 256;
        gIso.entries = (IsoEntry*)realloc(gIso.entries, (size_t)gIso.cap * sizeof(IsoEntry));
    }
    size_t n = strlen(path) + 1;
    gIso.entries[gIso.count].path = (char*)malloc(n);
    memcpy(gIso.entries[gIso.count].path, path, n);
    gIso.entries[gIso.count].offset = off;
    gIso.entries[gIso.count].size = size;
    ++gIso.count;
}

static int mountIso(const char* isoPath) {
    FILE* fp = fopen(isoPath, "rb");
    if (!fp) return 0;

    unsigned char hdr[0x440];
    if (fread(hdr, 1, sizeof(hdr), fp) != sizeof(hdr)) { fclose(fp); return 0; }

    // GameCube magic word at 0x1C: 0xC2339F3D. Reject anything else.
    if (be32(hdr + 0x1C) != 0xC2339F3Du) { fclose(fp); return 0; }

    unsigned fstOff  = be32(hdr + 0x424);
    unsigned fstSize = be32(hdr + 0x428);
    if (fstSize < 12 || fstSize > 16u * 1024 * 1024) { fclose(fp); return 0; }

    unsigned char* fst = (unsigned char*)malloc(fstSize);
    if (!fst) { fclose(fp); return 0; }
    if (fseek(fp, (long)fstOff, SEEK_SET) != 0 || fread(fst, 1, fstSize, fp) != fstSize) {
        free(fst); fclose(fp); return 0;
    }

    unsigned numEntries = be32(fst + 8);           // root entry's length = entry count
    if ((unsigned long)numEntries * 12u > fstSize) { free(fst); fclose(fp); return 0; }
    const char* strTab = (const char*)fst + (size_t)numEntries * 12u;

    memcpy(gIso.gameCode, hdr, 6);
    gIso.gameCode[6] = 0;

    // Walk entries building full paths; directories nest via their end-index.
    char path[1024]; int pathLen = 0;
    struct { unsigned end; int pathLen; } stack[64]; int sp = 0;

    for (unsigned i = 1; i < numEntries; ++i) {
        while (sp > 0 && stack[sp - 1].end <= i) { pathLen = stack[sp - 1].pathLen; --sp; }
        const unsigned char* e = fst + (size_t)i * 12u;
        int isDir = e[0];
        unsigned nameOff = be24(e + 1);
        unsigned dataOff = be32(e + 4);
        unsigned size    = be32(e + 8);
        const char* name = strTab + nameOff;
        if (isDir) {
            if (sp < 64) {
                stack[sp].end = size;
                stack[sp].pathLen = pathLen;
                ++sp;
            }
            int w = snprintf(path + pathLen, sizeof(path) - pathLen, "%s/", name);
            if (w > 0) pathLen += w;
        } else {
            char full[1200];
            snprintf(full, sizeof(full), "%.*s%s", pathLen, path, name);
            isoAddEntry(full, dataOff, size);
        }
    }

    free(fst);
    gIso.fp = fp;
    gIso.mounted = 1;
    printf("[dvd] mounted ISO %s: game=%s, %d files\n", isoPath, gIso.gameCode, gIso.count);
    return 1;
}

static const IsoEntry* isoFind(const char* path) {
    while (*path == '/' || *path == '\\') ++path;
    for (int i = 0; i < gIso.count; ++i)
        if (pathEqual(gIso.entries[i].path, path)) return &gIso.entries[i];
    return NULL;
}

// --- directory mode helpers ------------------------------------------------
static void resolvePath(char* out, size_t outSize, const char* rel) {
    while (*rel == '/' || *rel == '\\') ++rel;
    snprintf(out, outSize, "%s/%s", gDiscRoot, rel);
    for (char* p = out; *p; ++p) if (*p == '\\') *p = '/';
}

// --- name table / manifest -------------------------------------------------
int dvd_shim_setName(int id, const char* relPath) {
    if (id < 0 || id >= DVD_MAX_FILE_ID || !relPath) return 0;
    free(gNameTable[id]);
    size_t n = strlen(relPath) + 1;
    gNameTable[id] = (char*)malloc(n);
    if (!gNameTable[id]) return 0;
    memcpy(gNameTable[id], relPath, n);
    return 1;
}

static void loadManifest(void) {
    char path[600];
    if (gIso.mounted) return; // manifest lives beside an extracted dir, not in an ISO
    snprintf(path, sizeof(path), "%s/filetable.txt", gDiscRoot);
    FILE* f = fopen(path, "r");
    if (!f) return;
    char line[700];
    int count = 0;
    while (fgets(line, sizeof(line), f)) {
        if (line[0] == '#' || line[0] == '\n' || line[0] == '\r') continue;
        int id = -1; char rel[512];
        if (sscanf(line, "%d %511s", &id, rel) == 2 && dvd_shim_setName(id, rel)) ++count;
    }
    fclose(f);
    printf("[dvd] loaded %d entries from filetable.txt\n", count);
}

// --- init / shutdown -------------------------------------------------------
static int isRegularFile(const char* p) {
    FILE* f = fopen(p, "rb");
    if (!f) return 0;
    // A directory won't open in "rb" on Windows; on POSIX it might, so also reject
    // zero-length reads is not reliable - the ISO magic check in mountIso is the
    // real gate. Here we just confirm it opens as a file.
    fclose(f);
    return 1;
}

int dvd_shim_init(const char* discRoot) {
    if (discRoot && discRoot[0]) {
        snprintf(gDiscRoot, sizeof(gDiscRoot), "%s", discRoot);
        for (char* p = gDiscRoot; *p; ++p) if (*p == '\\') *p = '/';
    }
    // Try ISO first if the path is a file; mountIso validates the GC magic.
    if (isRegularFile(gDiscRoot)) {
        if (mountIso(gDiscRoot)) { return 1; }
    }
    loadManifest();
    printf("[dvd] disc root (dir): %s\n", gDiscRoot);
    return 1;
}

void dvd_shim_shutdown(void) {
    for (int i = 0; i < DVD_MAX_FILE_ID; ++i) {
        free(gNameTable[i]); gNameTable[i] = NULL;
        free(gCache[i]);     gCache[i] = NULL;
        gCacheSize[i] = 0;
    }
    if (gIso.entries) {
        for (int i = 0; i < gIso.count; ++i) free(gIso.entries[i].path);
        free(gIso.entries);
    }
    if (gIso.fp) fclose(gIso.fp);
    memset(&gIso, 0, sizeof(gIso));
}

// --- disc introspection ----------------------------------------------------
// Find the first ISO file whose basename starts with `prefix` and ends with `suffix`, optionally
// under directory `dir` (case-insensitive). Writes the full path into out. Returns 1 on success.
// Used to locate a map's mod<N>.zlb.bin without hardcoding the number per map.
int dvd_shim_findFile(const char* dir, const char* prefix, const char* suffix, char* out, int outSize) {
    if (!gIso.mounted) return 0;
    size_t dl = dir ? strlen(dir) : 0, pl = strlen(prefix), sl = strlen(suffix);
    for (int i = 0; i < gIso.count; ++i) {
        const char* p = gIso.entries[i].path;
        const char* base = p; for (const char* q = p; *q; ++q) if (*q=='/'||*q=='\\') base = q+1;
        if (dir) { if ((size_t)(base - p) < dl+1) continue;
            int ok = 1; for (size_t k=0;k<dl;++k){ char a=p[k],b=dir[k]; if(a>='A'&&a<='Z')a+=32; if(b>='A'&&b<='Z')b+=32; if(a!=b){ok=0;break;} }
            if (!ok || (p[dl] != '/' && p[dl] != '\\')) continue; }
        size_t bl = strlen(base);
        if (bl < pl + sl) continue;
        int ok = 1;
        for (size_t k=0;k<pl;++k){ char a=base[k],b=prefix[k]; if(a>='A'&&a<='Z')a+=32; if(b>='A'&&b<='Z')b+=32; if(a!=b){ok=0;break;} }
        if (ok) for (size_t k=0;k<sl;++k){ char a=base[bl-sl+k],b=suffix[k]; if(a>='A'&&a<='Z')a+=32; if(b>='A'&&b<='Z')b+=32; if(a!=b){ok=0;break;} }
        if (ok) { snprintf(out, outSize, "%s", p); return 1; }
    }
    return 0;
}

int         dvd_shim_isIso(void)        { return gIso.mounted; }
const char* dvd_shim_gameCode(void)     { return gIso.mounted ? gIso.gameCode : ""; }
int         dvd_shim_fileCount(void)    { return gIso.count; }
const char* dvd_shim_fileName(int idx, int* outSize) {
    if (idx < 0 || idx >= gIso.count) { if (outSize) *outSize = 0; return NULL; }
    if (outSize) *outSize = (int)gIso.entries[idx].size;
    return gIso.entries[idx].path;
}

// --- RETAIL DATA FROM THE PLAYER'S DISC (vita fork, 2026-10-27, the public release) ---------------------------------
// The binary carries NO retail bytes: gsae01_rev1_data.c / gsae01_string_pools.S are zero-filled (retail_runtime.py) and
// stfx_retail_table says where each object's bytes are in main.dol. This copies them from the disc image the player supplies,
// swapping each `elem`-byte element to host order (what gen_data.py did on the developer's host), and first CHECKS the image:
// game id GSAE01, disc version 1, and an FNV-1a 64 of main.dol equal to the Rev 1 disc's. Idempotent; returns STFX_DISC_*.
// The table is WEAK so the host tools that link dvd_shim without the game data still link; the Vita build gate checks it is there.
typedef struct { void* dst; unsigned addr, size; unsigned char elem; } StfxRetailEnt;
extern const StfxRetailEnt stfx_retail_table[] __attribute__((weak));
extern const unsigned stfx_retail_n __attribute__((weak));
#define STFX_DOL_FNV 0x22b046a1f7680feeull     // main.dol of GSAE01 rev 1 (3,402,176 bytes at 0x1e000), host/engine/retail_runtime.py notes
static int gRetailLoaded;
int stfx_retail_load(const char* iso, char* why, int cap) {
    if (gRetailLoaded) return STFX_DISC_OK;
    if (why && cap > 0) why[0] = 0;
    FILE* f = iso ? fopen(iso, "rb") : NULL;
    if (!f) { if (why) snprintf(why, (size_t)cap, "no disc image at %s", iso ? iso : "(null)"); return STFX_DISC_MISSING; }
    unsigned char hd[0x440], dh[0x100];
    int rc = STFX_DISC_OK;
    if (fread(hd, 1, sizeof hd, f) != sizeof hd) rc = STFX_DISC_BADREAD;
    else if (memcmp(hd, "GSAE01", 6) != 0) { rc = STFX_DISC_WRONG_GAME; if (why) snprintf(why, (size_t)cap, "game id %.6s, want GSAE01", (const char*)hd); }
    else if (hd[7] != 1) { rc = STFX_DISC_WRONG_REV; if (why) snprintf(why, (size_t)cap, "disc version %u, want 1 (Rev 1)", hd[7]); }
    const unsigned dol = rc ? 0 : be32(hd + 0x420);
    unsigned offs[18], addrs[18], sizes[18], end = 0x100;
    if (!rc && (fseek(f, (long)dol, SEEK_SET) || fread(dh, 1, sizeof dh, f) != sizeof dh)) rc = STFX_DISC_BADREAD;
    if (!rc) {
        for (int i = 0; i < 18; ++i) { offs[i] = be32(dh + 4*i); addrs[i] = be32(dh + 0x48 + 4*i); sizes[i] = be32(dh + 0x90 + 4*i); if (sizes[i] && offs[i] + sizes[i] > end) end = offs[i] + sizes[i]; }
        unsigned char* img = (unsigned char*)malloc(end);
        if (!img || fseek(f, (long)dol, SEEK_SET) || fread(img, 1, end, f) != end) rc = STFX_DISC_BADREAD;
        else {
            unsigned long long x = 0xcbf29ce484222325ull;
            for (unsigned i = 0; i < end; ++i) x = (x ^ img[i]) * 0x100000001b3ull;
            if (x != STFX_DOL_FNV) { rc = STFX_DISC_WRONG_REV; if (why) snprintf(why, (size_t)cap, "main.dol fingerprint %016llx, want %016llx (a modified or different revision disc)", x, STFX_DOL_FNV); }
            unsigned n = &stfx_retail_n ? stfx_retail_n : 0u, done = 0;
            for (unsigned e = 0; !rc && e < n; ++e) {
                const StfxRetailEnt* t = &stfx_retail_table[e];
                int s = -1;
                for (int i = 0; i < 18; ++i) if (sizes[i] && t->addr >= addrs[i] && t->addr + t->size <= addrs[i] + sizes[i]) { s = i; break; }
                if (s < 0) { rc = STFX_DISC_BADREAD; if (why) snprintf(why, (size_t)cap, "table entry %u (%08x+%u) is in no DOL section", e, t->addr, t->size); break; }
                const unsigned char* src = img + offs[s] + (t->addr - addrs[s]);
                unsigned char* dst = (unsigned char*)t->dst;
                const unsigned el = t->elem ? t->elem : 1u;
                for (unsigned k = 0; k < t->size; k += el) for (unsigned b = 0; b < el && k + b < t->size; ++b) dst[k + b] = src[k + (el - 1 - b)];
                ++done;
            }
            if (!rc) printf("[retail] %u objects filled from the player's main.dol (%u bytes, fingerprint ok)%s\n", done, end, n ? "" : " -- NO TABLE LINKED");
        }
        free(img);
    }
    if (rc == STFX_DISC_BADREAD && why && !why[0]) snprintf(why, (size_t)cap, "could not read the disc image (truncated or not a GameCube image)");
    fclose(f);
    if (!rc) gRetailLoaded = 1;
    else printf("[retail] disc check FAILED (%d): %s\n", rc, why ? why : "");
    return rc;
}

// --- DVD API ---------------------------------------------------------------
void DVDInit(void) {
    // The real game calls DVDInit() with no path; the "disc" is the ISO configured via
    // STAIRFAX_ISO (so the game's own boot path mounts it without extra wiring). A tool
    // that already called dvd_shim_init() explicitly leaves gIso mounted and this is a
    // no-op.
    if (gIso.count == 0) {
        const char* iso = getenv("STAIRFAX_ISO");
        if (iso && *iso) dvd_shim_init(iso);
    }
    // the retail data, if the platform entry has not loaded it already (the Vita does, before the game's main, with an error screen)
    { const char* iso = getenv("STAIRFAX_ISO"); if (iso && *iso) stfx_retail_load(iso, NULL, 0); }
}
void DVDSetAutoInvalidation(int32_t enable) { gAutoInvalidate = enable; }
int32_t DVDGetDriveStatus(void) { return 0; }
int32_t DVDGetCommandBlockStatus(DVDCommandBlock* block) { (void)block; return 0; }

// vita fork: a miss USED TO BE SILENT. On the Vita the disc directory held 8 of the disc's
// 4,070 files, and the game hung after VIInit with nothing in the log -- a missing file
// the game retries forever is indistinguishable from any other hang unless misses print.
unsigned dvd_open_misses;
// Host FILE* per open DVDFileInfo (directory mode). The struct itself is the SDK's 0x3C
// layout, which has no room for it (see dvd_shim.h). Keyed by address; freed on close.
#define DVD_HOST_SLOTS 128
static struct { const DVDFileInfo* fi; FILE* f; } gHostOpen[DVD_HOST_SLOTS];
unsigned dvd_host_slots_full;
static FILE* hostGet(const DVDFileInfo* fi) {
    for (int i = 0; i < DVD_HOST_SLOTS; ++i) if (gHostOpen[i].fi == fi) return gHostOpen[i].f;
    return NULL;
}
static int hostSet(const DVDFileInfo* fi, FILE* f) {   // f == NULL clears
    int freeSlot = -1;
    for (int i = 0; i < DVD_HOST_SLOTS; ++i) {
        if (gHostOpen[i].fi == fi) { if (f) gHostOpen[i].f = f; else gHostOpen[i].fi = NULL, gHostOpen[i].f = NULL; return 1; }
        if (!gHostOpen[i].fi && freeSlot < 0) freeSlot = i;
    }
    if (!f) return 1;
    if (freeSlot < 0) { ++dvd_host_slots_full; printf("[dvd] host file table FULL (%d open)\n", DVD_HOST_SLOTS); return 0; }
    gHostOpen[freeSlot].fi = fi; gHostOpen[freeSlot].f = f;
    return 1;
}
// The host path of each directory-mode open file: the disc audio stream (hal/dtk_stream.c) reads the file on its own
// thread through its own handle, so it needs the NAME, not this FILE*.
static char gHostPath[DVD_HOST_SLOTS][200];
static void hostPathSet(const DVDFileInfo* fi, const char* path) {
    for (int i = 0; i < DVD_HOST_SLOTS; ++i) if (gHostOpen[i].fi == fi) { snprintf(gHostPath[i], sizeof gHostPath[i], "%s", path); return; }
}
static const char* hostPathGet(const DVDFileInfo* fi) {
    for (int i = 0; i < DVD_HOST_SLOTS; ++i) if (gHostOpen[i].fi == fi) return gHostPath[i];
    return NULL;
}
static void dvdMiss(const char* path) {
    if (++dvd_open_misses <= 64) printf("[dvd] MISS #%u: %s\n", dvd_open_misses, path);
    else if (dvd_open_misses == 65) printf("[dvd] MISS: further misses counted, not printed\n");
}
int32_t DVDOpen(const char* path, DVDFileInfo* info) {
    if (!path || !info) return 0;
    hostSet(info, NULL);
    info->startAddr = 0;
    if (gIso.mounted) {
        const IsoEntry* e = isoFind(path);
        if (!e) { info->length = 0; dvdMiss(path); return 0; }
        info->length = (int32_t)e->size;
        info->startAddr = (int32_t)e->offset;  // absolute ISO byte offset
        // hostFile stays NULL -> ISO-backed; reads use the shared image handle.
        return 1;
    }
    char hostPath[260];
    resolvePath(hostPath, sizeof(hostPath), path);
    FILE* f = fopen(hostPath, "rb");
    if (!f) { info->length = 0; dvdMiss(path); return 0; }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (!hostSet(info, f)) { fclose(f); info->length = 0; return 0; }
    hostPathSet(info, hostPath);
    info->length = (int32_t)sz;
    return 1;
}

int32_t DVDClose(DVDFileInfo* info) {
    if (!info) return 0;
    { FILE* hf = hostGet(info); if (hf) { fclose(hf); hostSet(info, NULL); } } // ISO handle is shared: no close
    return 1;
}

extern void stfx_mldf_on_read(void* buf, int size) __attribute__((weak));
int32_t DVDRead(DVDFileInfo* info, void* buf, int32_t size, int32_t offset) {
    if (!info || !buf || size < 0) return -1;
    FILE* f;
    long base;
    if (hostGet(info)) {           // directory mode
        f = hostGet(info);
        base = 0;
    } else if (gIso.mounted) {     // ISO mode
        f = gIso.fp;
        base = (long)(unsigned)info->startAddr;
    } else {
        return -1;
    }
    if (fseek(f, base + offset, SEEK_SET) != 0) return -1;
    size_t got = fread(buf, 1, (size_t)size, f);
    if ((int32_t)got < size) memset((char*)buf + got, 0, (size_t)size - got); // pad past EOF
    // A read at offset 0 may be filling one of the game's RESIDENT file buffers
    // (gResourceFileBuffers[id]): the endian layer swaps it by that file's rule. Weak, so the
    // viewer builds (no game, no mldf_endian.c) link unchanged. docs/47 run 10.
    if (offset == 0 && stfx_mldf_on_read) stfx_mldf_on_read(buf, size);
    return size;
}

int32_t DVDReadAsyncPrio(DVDFileInfo* info, void* buf, int32_t size, int32_t offset, DVDCallback cb, int32_t prio) {
    (void)prio;
    int32_t r = DVDRead(info, buf, size, offset);
    if (cb) cb(r, info);
    return r < 0 ? 0 : 1;
}

// --- high-level file loaders -----------------------------------------------
void* loadFileByPath(char* path, int* outSize, int unused) {
    (void)unused;
    if (outSize) *outSize = 0;
    DVDFileInfo info;
    if (!DVDOpen(path, &info)) return NULL;
    int size = info.length;
    int32_t aligned = (size + 0x1f) & ~0x1f;
    void* buf = malloc((size_t)aligned);
    if (!buf) { DVDClose(&info); return NULL; }
    if (DVDRead(&info, buf, aligned, 0) < 0) { free(buf); DVDClose(&info); return NULL; }
    DVDClose(&info);
    if (outSize) *outSize = size;
    return buf;
}

void* fileLoad(int id, int heap) {
    (void)heap;
    if (id < 0 || id >= DVD_MAX_FILE_ID) return NULL;
    if (gCache[id]) return gCache[id];
    if (!gNameTable[id]) { fprintf(stderr, "[dvd] fileLoad(%d): no name mapping\n", id); return NULL; }
    int size = 0;
    void* buf = loadFileByPath(gNameTable[id], &size, 0);
    if (!buf) return NULL;
    gCache[id] = buf;
    gCacheSize[id] = size;
    return buf;
}

int fileLoadToBuffer(int id, void* buffer) {
    if (id < 0 || id >= DVD_MAX_FILE_ID || !buffer) return 0;
    if (gCache[id]) { memcpy(buffer, gCache[id], (size_t)gCacheSize[id]); return gCacheSize[id]; }
    if (!gNameTable[id]) return 0;
    DVDFileInfo info;
    if (!DVDOpen(gNameTable[id], &info)) return 0;
    int32_t r = DVDRead(&info, buffer, info.length, 0);
    int len = info.length;
    DVDClose(&info);
    return r < 0 ? 0 : len;
}

// Register an already-loaded file buffer under an id so fileLoad*/fileLoadToBufferOffset
// can serve slices of it (used to hand dvd the MLDF buffers game_assetfile loaded).
void dvd_register_buffer(int id, void* buf, int size) {
    if (id < 0 || id >= DVD_MAX_FILE_ID) return;
    gCache[id] = buf; gCacheSize[id] = size;
}

// Optional per-id post-load fixups (e.g. byte-swap an OBJECTS.bin ObjDef so the
// recompiled loadObjectFile reads it in host order). Weak-ish: only for known ids.
extern void bswapObjDef(void* p);
#define MLDF_FILEID_OBJECTS_BIN 0x3e

int fileLoadToBufferOffset(int id, void* dst, int offset, int size) {
    if (size == 0 || id < 0 || id >= DVD_MAX_FILE_ID || !dst) return 0;
    int got = 0;
    if (gCache[id]) {
        if (offset + size > gCacheSize[id]) return 0;
        memcpy(dst, (char*)gCache[id] + offset, (size_t)size);
        got = size;
    } else if (gNameTable[id]) {
        DVDFileInfo info;
        if (!DVDOpen(gNameTable[id], &info)) return 0;
        int32_t r = DVDRead(&info, dst, size, offset);
        DVDClose(&info);
        got = r < 0 ? 0 : size;
    }
    if (got && id == MLDF_FILEID_OBJECTS_BIN) bswapObjDef(dst);  // BE ObjDef -> host order
    return got;
}

int32_t fileGetSize(int id) {
    if (id < 0 || id >= DVD_MAX_FILE_ID) return 0;
    if (gCache[id]) return gCacheSize[id];
    if (!gNameTable[id]) return 0;
    DVDFileInfo info;
    if (!DVDOpen(gNameTable[id], &info)) return 0;
    int32_t len = info.length;
    DVDClose(&info);
    return len;
}

// NOTE: loadFileByPathAsync lives in bridge/game_assetfile.c, not here: the game
// mm_free's the buffer it returns, so it must be mmAlloc'd, and this HAL lib does not
// (and must not) link the mm shim.

int32_t DVDCancelAsync(DVDCommandBlock* block, void* callback) {
    (void)block; (void)callback;  // sync IO: nothing in flight to cancel
    return 1;
}

void setFileInfo(DVDFileInfo* fileInfo) { (void)fileInfo; }

// --- priority read, cancel, disk check, and DISC AUDIO STREAMING (vita fork) ---------
// DVDReadPrio: DVDRead (reads are synchronous here; priority has nothing to order).
int32_t DVDReadPrio(DVDFileInfo* info, void* addr, int32_t length, int32_t offset, int32_t prio) {
    (void)prio;
    return DVDRead(info, addr, length, offset);
}
int32_t DVDCancel(DVDCommandBlock* block) { (void)block; return 0; }   // nothing is ever in flight
int     DVDCheckDisk(void) { return 1; }                               // the disc is always present

// DISC AUDIO STREAMING (dialogue and streamed music: src/main/audio_stream.c, streams/*/*.adp) -- hal/dtk_stream.c
// since 2026-10-11. Until then these four were stubs: no sound, and the status poll said "in progress" for ever.
//
// CALLBACKS: each call completes SYNCHRONOUSLY and runs the game's callback inside the call, on the calling game
// thread -- upstream's DVDReadAsyncPrio convention, and what the stubs did. The stream code tolerates it: it sets its
// state (gAudioStreamDvdState = 1) BEFORE calling DVDPrepareStreamAsync, not after. Nothing here is ever called from
// the audio thread or from dtk_stream.c's reader thread.
//
// THE FOURTH CALL IS MISNAMED IN THE DECOMP: "DVDGetStreamPlayAddrAsync" issues drive command 9 (src/dolphin/dvd/
// dvd.c:660: DVDLowRequestAudioStatus(0)), which is the SDK's DVDGetStreamErrorStatusAsync -- the answer is the
// stream STATUS, 1 while a stream is active and 0 once it has ended or was cancelled, not an address. The game polls it
// every frame (fileio.c dvdCheckError) and AudioStream_PlayAddrCallback takes a zero low byte as "the stream is over":
// THAT is how a line of dialogue ends. The stubs answered 1 for ever (dtk_off.txt '2' still does).
typedef void (*DVDCBCallbackPort)(int32_t result, DVDCommandBlock* block);
unsigned dvd_stream_prepares, dvd_stream_cancels;
extern void     dtk_stream_prepare(const char* file, uint32_t base, uint32_t length, const char* label);
extern void     dtk_stream_stop_at_end(void);
extern void     dtk_stream_cancel(void);
extern uint32_t dtk_stream_status(void);
int DVDPrepareStreamAsync(DVDFileInfo* info, uint32_t length, uint32_t offset, DVDCallback cb) {
    ++dvd_stream_prepares;
    if (info) {
        const char* file = hostPathGet(info);     // directory mode: the .adp itself
        const char* label = file;
        uint32_t base = offset;
        if (!file && gIso.mounted) {              // image mode: a byte range of the ISO
            file = gDiscRoot; base = info->startAddr + offset; label = "?";
            for (int i = 0; i < gIso.count; ++i) if (gIso.entries[i].offset == info->startAddr) { label = gIso.entries[i].path; break; }
        }
        if (length == 0 && (uint32_t)info->length > offset) length = (uint32_t)info->length - offset;   // dvdfs.c:375: 0 = to the end of the file
        dtk_stream_prepare(file ? file : "", base, length, label ? label : "?");
    }
    if (cb) cb(0, info);
    return 1;
}
int DVDCancelStreamAsync(DVDCommandBlock* block, DVDCBCallbackPort cb) {
    ++dvd_stream_cancels;
    dtk_stream_cancel();
    if (cb) cb(0, block);
    return 1;
}
int DVDStopStreamAtEndAsync(DVDCommandBlock* block, DVDCBCallbackPort cb) {
    dtk_stream_stop_at_end();
    if (cb) cb(0, block);
    return 1;
}
int DVDGetStreamPlayAddrAsync(DVDCommandBlock* block, DVDCBCallbackPort cb) {   // really the stream STATUS: see above
    uint32_t st = dtk_stream_status();
    if (cb) cb((int32_t)st, block);
    return 1;
}
