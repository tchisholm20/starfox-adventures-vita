#pragma once
// dvd_shim.h - DVD / fileio on the host filesystem.
//
// The game reads its data through the Dolphin DVD API (DVDOpen/Read/Close by path)
// and the high-level fileLoad(id) wrappers (id -> name table -> DVD -> cached
// buffer). The console reads sectors off the disc; the host reads files from an
// extracted disc directory (the "disc root"). Reads return raw bytes - disc data
// is big-endian and stays that way here; consumers byte-swap.
//
// DVDFileInfo keeps the one field game code reads (.length) plus host internals.
// Binary layout need not match the console (this is a native build); field names
// that game TUs reference must.

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// vita fork: the SDK's EXACT layout (dolphin/dvd.h), because game TUs are compiled against
// THAT header, not this one: they read fileInfo.length at 0x34 and size pools of these
// structs as 0x40 bytes. Upstream's port layout (length at 0, a 260-byte host path inline,
// 272 bytes) made every length the game read a slice of a path string, and overran the
// game's DVDFileInfo pool (device run 6: a free-list link reading "ux0:"). The host's open
// FILE* now lives in a side table in dvd_shim.c, keyed by the struct's address.
typedef struct DVDFileInfo {
    unsigned char cb[0x30];   // DVDCommandBlock: opaque to the port, the game's own
    uint32_t startAddr;       // 0x30: absolute ISO byte offset (image mode)
    int32_t  length;          // 0x34: file size in bytes
    void*    callback;        // 0x38
} DVDFileInfo;
#if defined(__vita__) || (defined(__SIZEOF_POINTER__) && __SIZEOF_POINTER__ == 4)
#ifdef __cplusplus
static_assert(offsetof(DVDFileInfo, length) == 0x34 && sizeof(DVDFileInfo) == 0x3C,
               "DVDFileInfo must match the SDK layout the game is compiled against");
#else
_Static_assert(offsetof(DVDFileInfo, length) == 0x34 && sizeof(DVDFileInfo) == 0x3C,
               "DVDFileInfo must match the SDK layout the game is compiled against");
#endif
#endif

typedef struct DVDCommandBlock DVDCommandBlock;
typedef void (*DVDCallback)(int32_t result, DVDFileInfo* info);

// Dolphin DVD API (host-backed). Return conventions match dolphin/dvd.h:
//   DVDOpen  -> nonzero on success, 0 on failure
//   DVDRead  -> bytes transferred, or -1 on error
//   DVDReadAsyncPrio -> nonzero if queued (callback fires with the result)
void    DVDInit(void);
int32_t DVDOpen(const char* path, DVDFileInfo* info);
int32_t DVDClose(DVDFileInfo* info);
int32_t DVDRead(DVDFileInfo* info, void* buf, int32_t size, int32_t offset);
int32_t DVDReadAsyncPrio(DVDFileInfo* info, void* buf, int32_t size, int32_t offset, DVDCallback cb, int32_t prio);
int32_t DVDGetDriveStatus(void);
int32_t DVDGetCommandBlockStatus(DVDCommandBlock* block);
void    DVDSetAutoInvalidation(int32_t enable);

// Game high-level wrappers (replaces src/main/fileio.c + pi_dolphin.c file path).
void*   loadFileByPath(char* path, int* outSize, int unused);
void*   fileLoad(int id, int heap);
int     fileLoadToBuffer(int id, void* buffer);
int     fileLoadToBufferOffset(int id, void* dst, int offset, int size);
void    dvd_register_buffer(int id, void* buf, int size);
int32_t fileGetSize(int id);

// Host control. discRoot may be an extracted-disc directory OR a GameCube ISO/GCM
// image file - dvd_shim_init auto-detects and, for an image, parses the disc FST
// so DVDOpen/fileLoad read files straight out of the ISO.
int  dvd_shim_init(const char* discRoot);
// The retail data the binary does not carry (2026-10-27): checks the image (GSAE01, version 1, main.dol fingerprint) and fills
// stfx_retail_table from it. Idempotent. `why` (may be NULL) gets a one-line reason on failure.
enum { STFX_DISC_OK = 0, STFX_DISC_MISSING = 1, STFX_DISC_WRONG_GAME = 2, STFX_DISC_WRONG_REV = 3, STFX_DISC_BADREAD = 4 };
int  stfx_retail_load(const char* iso, char* why, int cap);
void dvd_shim_shutdown(void);
int  dvd_shim_setName(int id, const char* relPath); // register/override an id -> path mapping

// Disc introspection (populated when an ISO/GCM is mounted).
int         dvd_shim_isIso(void);
const char* dvd_shim_gameCode(void);          // 6-char disc game code, e.g. "GSAE01"
int         dvd_shim_fileCount(void);          // number of files in the FST
const char* dvd_shim_fileName(int idx, int* outSize); // FST entry idx -> path (+ size)

#ifdef __cplusplus
}
#endif
