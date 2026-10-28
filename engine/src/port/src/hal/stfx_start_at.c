// stfx_start_at.c - developer START POINT: skip the title, GAME SELECT and the ~10-minute prologue.
// Built with the game units' flags by compile_all.sh (globbed from src/port/src/hal). 2026-10-11.
//
// ux0:data/stairfax/start_at.txt (absent = a normal boot; nothing below runs). Whitespace-separated words, '#' to end of
// line is a comment, case-insensitive keywords:
//
//     warp <n>                   enter at WARPTAB.bin entry n (position, layer, heading) -- the table the game's own
//                                warpToMap(idx, ..) reads; the MAP is whatever mapCoordsToId() says holds that point
//     map <id>                   OPTIONAL cross-check: the MAPINFO.bin index the warp must resolve to (a mismatch is
//                                logged and the start proceeds; the game decides the map by coordinates)
//     pos <x> <y> <z> [layer <l>] [angle <a>]     a raw position instead of a warp (layer -2..2, angle 0..255)
//     char krystal|fox|0|1       which playable character (SaveGameData.currentCharacter). INFERRED mapping:
//                                0 spawns object def 0x1F ("Krystal"), 1 spawns def 0 ("Sabre", the Fox of the final
//                                game); default = whatever the starting save says (0 for a new game)
//     chapter <n>                n = 2..5: the game's OWN chapter-start save image /savegame/save<n>.bin from the disc
//                                (the one its hidden chapter-select menu loads), instead of a blank new game; a
//                                warp/pos given as well then overrides only the position
//     bits <id>=<v>,<id>=<v>...  game bits set BEFORE the load (BITTABLE.bin ids; only the SAVED banks 1-3 can be set
//                                this way -- bank 0 is zeroed by every load, such a bit is skipped and said so)
//     report                     print the player's position + map in every status block (alone = a normal boot that
//                                tells you the numbers for a later `pos`/`warp`)
//     act <map>=<n>,...          map "acts" (SaveGame_gplaySetAct): writes the act's game bit, same restriction
//
// HOW IT ENTERS THE GAME. The hook (reapply_hooks.py, TitleMenu_run in dlls/engine/52_n_attractmode) fires once the title
// camera is up. It runs what the file-select does for a new game -- gplayNewGame(.., -1), no card write -- edits the
// resulting WORK BUFFER (the 0x6EC-byte image the card holds: character position/layer/heading, current character, bit
// banks), then lets the title's OWN exit path run (releaseMovieBuffers, mapUnload(0x3D), titleDoLoadSave ->
// gotoSavegame -> loadMapForCurrentSaveGame -> mapLoadByCoords). Nothing is poked into a running game.
//
// WHAT IT CANNOT KNOW (a real playthrough sets these; this does not unless you list them): the game bits a cutscene sets
// when it ends, per-map object state, inventory/health beyond a new game's, the act of every map beyond "act 1" (the new
// game sets act 1 everywhere; the Krazoa Palace, ThornTail Hollow ... are moved to later acts by story events -- see the
// table in the report / docs), the Tricky companion's state, time of day. Every start prints these caveats.
//
// SAVING. The game's save is slot 0 of the single card file: a game started here writes slot 0 when the pause menu saves.
// Back the card up first (host/engine/saves.py pull).
//
// WHERE THINGS ARE (derived 2026-10-11 from the Rev 1 disc: WARPTAB.bin 16-byte records {f32 x,y,z; s16 layer, angle}; map of a
// point = the same test mapCoordsToId() makes: globalma.bin {originX,originZ,layer,mapId,adj1,adj2} + MAPS.tab/MAPS.bin cell
// bitmaps, cell = floor(x/640), z likewise, layer must equal the map's layer; names = MAPINFO.bin[mapId].name. All files are
// big-endian on the disc. "heading" is the warp's angle byte as the game stores it (s8): 192 = -64.)
//
//   place                                  map  how to start here
//   -------------------------------------  ---  ------------------------------------------------------------------------
//   Ship Battle / the prologue (new game)    0  no start_at (the new-game default (570.6,-82,15790.8) layer 0 is in map 0)
//   galleon on foot                          ?   INFERRED to be inside map 0 (no separate galleon map is in the prologue);
//                                                use `report` while standing there, then `pos x y z`. Maps 80 galleonship /
//                                                81 cfgalleon are CloudRunner/Cape Claw sub-maps, not this.
//   Krazoa Palace (entrance; chapters 2-5      11  warp 40 = (10688,-84,1495) heading 192.  Also map 11: warps 6, 32, 34, 65, 78
//     put character 0 here)
//   ThornTail Hollow                           7  warps 3, 15 (heading 235), 52, 102, 108;  underground (map 8, layer -1): warp 95
//     (chapters 2-5 put the current char          chapter saves: char 1 at (-5552.6,-781,-3281.3) [ch 2], (-5572.3,-780.9,-3276.5) [ch 3-5]
//      here, completion 54/94/126/158 of 187)
//   SnowHorn Wastes                           10  warps 66, 103
//   Moon Mountain Pass                        18  warps 16, 64
//   LightFoot Village                         14  warps 71, 80, 85
//   Cape Claw                                 29  warp 53
//   Volcano Force Point                        4  warps 72, 81, 122, 123, 124
//   DarkIce Mines - Top                       19  warp 119;  BOSS DarkIce (28, layer -2): warps 29, 30, 54, 92
//   Dragon Rock - Top / Bottom             2 / 52 warp 121 / 83 (layer -1)
//   CloudRunner Fortress / Race / Dungeon 12/43/16 warps 74, 99 / 73 / 0, 12
//   Ocean Force Point - Top / Bottom      50 / 21 warps 104, 115 / 75, 76, 105, 113, 114 (layer -1)
//   Walled City                               13  warps 19, 21, 70, 91, 120
//   Arwing levels (layer 2)       38,58-62        warps 50 (Andross), 111 (to Planet), 116 (DarkIce), 110 (Cloud), 118 (City), 117 (Dragon)
//   Great Fox 65 (warp 127), LinkA warpstone hub 66 (warp 126), Game Front = the title 63 (warp 18), World Map 41 (warp 96)
//   chapter select (the game's own, via /savegame/save<n>.bin):  chapter 2..5 -> Krazoa Palace (char 0) + ThornTail Hollow
//     (char 1, current) with the story bits of that chapter.
// Warp entries with x=z=0 are unused (1, 4, 7-11, 13, 14, 17, 20, 22-25, 27, 28, 31, 33, 35, 41, 44-49, 55-63, 67, 69, 77, 79, 82,
// 86, 88, 89, 97, 98, 100, 101, 106, 107, 112, 125).
//
// Examples (ux0:data/stairfax/start_at.txt):
//   warp 40 map 11 char 0              Krazoa Palace entrance, same character the chapter saves use there
//   chapter 3                          the game's chapter-3 image (ThornTail Hollow, 94/187 complete)
//   pos 570.6 -82 15790.8              the prologue start, explicitly
//   report                             a normal boot that prints the player's pos/map in every status block
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <ctype.h>
#include "types.h"

#ifndef SA_FILE
#define SA_FILE "ux0:data/stairfax/start_at.txt"
#endif

/* ---- game symbols (kept as plain externs: no game headers needed for these) ---------------------------------------- */
extern u8*  gSaveGameWorkBuffer;
extern int  gplayNewGame();                       /* K&R in 23.c: (char* name, s8 slot) */
extern void* loadFileByPath(char* path, int* outSize, int unused);
extern void mm_free(void* p);
extern void getTabEntry(void* dst, int fileId, int offset, int size);
extern int  mapCoordsToId(int x, int z, int layerIdx);
extern s8   gMapLayerOffsets[8];
extern s32  getCurMapLayer(void);
extern unsigned short gSaveGameMapActBits[];
typedef struct { unsigned short firstBit; unsigned char flags; unsigned char taskHintId; } SaGameBitDef;
extern SaGameBitDef* gGameBitTable;
extern short gGameBitCount;
extern unsigned char gSaveGameEnabled;

#define MLDF_WARPTAB 0x1c
#define MLDF_MAPINFO 0x1f

/* SaveGameData offsets (dlls/engine/23/23.c STATIC_ASSERTs) */
#define SG_ACTIVE      0x6ec
#define SG_CURCHAR     0x20
#define SG_NEWFILE     0x21
#define SG_CHARPOS     0x684            /* SaveGameCharacterPosition[2], 16 bytes each: f32 x,y,z; s8 angle,mapLayer,mapDataFileId,pad */

typedef struct { float x, y, z; short layer, angle; } SaWarp;      /* WarpDestination, in memory order (WARPTAB.bin is swapped on load) */

/* ---- the request -------------------------------------------------------------------------------------------------- */
#define MAXBITS 64
typedef struct { int id, val, isAct; } SaBit;
static struct {
    int state;                 /* 0 unread, 1 none/finished, 2 pending (title not up yet), 3 applied */
    int haveWarp, warp, haveMap, map, havePos, layer, angle, ch, chapter;
    float x, y, z;
    SaBit bits[MAXBITS]; int nbits;
    int reported;              /* the post-load report has been printed */
    int report;                /* 'report': print the player's position + map in every status block (to read off a pos/warp) */
    int appliedX, appliedZ, appliedLayer;
} G = { .ch = -1, .chapter = -1, .map = -1 };

static void sa_swap16(u8* p) { u8 t = p[0]; p[0] = p[1]; p[1] = t; }
static void sa_swap32(u8* p) { u8 t = p[0]; p[0] = p[3]; p[3] = t; t = p[1]; p[1] = p[2]; p[2] = t; }

/* The chapter images on the disc are the PowerPC (big-endian) in-memory SaveGameData; the port keeps native (little) order
 * in the card and in gSaveGameData (card_shim.c). Swap every multi-byte field of the 0x6EC active image; byte arrays (the
 * three game-bit banks at 0x24..0x168, 0x564..0x5D8, 0x5D8..0x684, the name, flags) are untouched. Offsets are the
 * STATIC_ASSERTed layout of SaveGameData / PlayerStatus / SaveGameEnvState; checked on the disc images (positions and
 * time-of-day decode to sane values). */
void stfx_swap_savegame_image(u8* s)
{
    int i;
    for (i = 0; i < 2; i++) { sa_swap16(s + i * 12 + 4); sa_swap16(s + i * 12 + 6); }     /* PlayerStatus.magic, maxMagic */
    for (i = 0; i < 0x3f; i++) { u8* p = s + 0x168 + i * 16; sa_swap32(p); sa_swap32(p + 4); sa_swap32(p + 8); sa_swap32(p + 12); }
    sa_swap32(s + 0x560);                                                                 /* playTime */
    for (i = 0; i < 2; i++) { u8* p = s + SG_CHARPOS + i * 16; sa_swap32(p); sa_swap32(p + 4); sa_swap32(p + 8); }
    sa_swap16(s + 0x6a4);                                                                 /* camActionNo */
    {   u8* e = s + 0x6a8;                                                                /* SaveGameEnvState */
        sa_swap32(e);
        sa_swap16(e + 4); sa_swap16(e + 6); sa_swap16(e + 0xa); sa_swap16(e + 0xc);
        sa_swap16(e + 0xe); sa_swap16(e + 0x10); sa_swap16(e + 0x12);
        for (i = 0; i < 9; i++) sa_swap32(e + 0x14 + i * 4);
    }
}

/* ---- parsing ------------------------------------------------------------------------------------------------------ */
static int sa_ieq(const char* a, const char* b) { for (; *a && *b; a++, b++) if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return 0; return *a == *b; }

static void sa_pairs(const char* t, int isAct)
{
    char buf[256]; char* p; strncpy(buf, t, sizeof buf - 1); buf[sizeof buf - 1] = 0;
    for (p = strtok(buf, ","); p; p = strtok(NULL, ",")) {
        char* eq = strchr(p, '=');
        if (G.nbits >= MAXBITS) { printf("[start_at] too many bits/acts (max %d); ignoring %s\n", MAXBITS, p); continue; }
        G.bits[G.nbits].id = (int)strtol(p, NULL, 0);
        G.bits[G.nbits].val = eq ? (int)strtol(eq + 1, NULL, 0) : 1;
        G.bits[G.nbits].isAct = isAct;
        G.nbits++;
    }
}

static int sa_read(void)
{
    FILE* f = fopen(SA_FILE, "r");
    char text[1024]; size_t n; char* tok[96]; int nt = 0, i; char* p;
    if (!f) return 0;
    n = fread(text, 1, sizeof text - 1, f); fclose(f); text[n] = 0;
    for (p = text; *p; ) {                                    /* strip comments */
        if (*p == '#') while (*p && *p != '\n') *p++ = ' ';
        else p++;
    }
    for (p = strtok(text, " \t\r\n"); p && nt < 96; p = strtok(NULL, " \t\r\n")) tok[nt++] = p;
    for (i = 0; i < nt; i++) {
        const char* k = tok[i];
        if (sa_ieq(k, "warp") && i + 1 < nt)      { G.haveWarp = 1; G.warp = (int)strtol(tok[++i], NULL, 0); }
        else if (sa_ieq(k, "map") && i + 1 < nt)  { G.haveMap = 1; G.map = (int)strtol(tok[++i], NULL, 0); }
        else if (sa_ieq(k, "chapter") && i + 1 < nt) G.chapter = (int)strtol(tok[++i], NULL, 0);
        else if (sa_ieq(k, "layer") && i + 1 < nt)   { G.layer = (int)strtol(tok[++i], NULL, 0); }
        else if (sa_ieq(k, "angle") && i + 1 < nt)   { G.angle = (int)strtol(tok[++i], NULL, 0); }
        else if (sa_ieq(k, "pos") && i + 3 < nt)  { G.havePos = 1; G.x = (float)atof(tok[i + 1]); G.y = (float)atof(tok[i + 2]); G.z = (float)atof(tok[i + 3]); i += 3; }
        else if (sa_ieq(k, "char") && i + 1 < nt) {
            const char* c = tok[++i];
            G.ch = sa_ieq(c, "krystal") ? 0 : sa_ieq(c, "fox") ? 1 : (int)strtol(c, NULL, 0);
            if (G.ch != 0 && G.ch != 1) { printf("[start_at] char %s is not krystal/fox/0/1; ignored\n", c); G.ch = -1; }
        }
        else if (sa_ieq(k, "report")) G.report = 1;
        else if (sa_ieq(k, "bits") && i + 1 < nt) sa_pairs(tok[++i], 0);
        else if (sa_ieq(k, "act") && i + 1 < nt)  sa_pairs(tok[++i], 1);
        else printf("[start_at] unknown/incomplete word '%s' ignored\n", k);
    }
    return 1;
}

/* ---- the hook's two entry points ---------------------------------------------------------------------------------- */
/* 1 exactly once: the request exists AND the title is up. Reads the file on the first call. */
int stfx_start_at_wanted(int titleReady)
{
    if (G.state == 0) {
        if (!sa_read()) { G.state = 1; return 0; }
        if (G.report && !G.haveWarp && !G.havePos && G.chapter < 2 && G.ch < 0 && !G.nbits) {
            G.state = 1; printf("[start_at] report only: a normal boot, with the player's position and map printed in every status block\n"); return 0;
        }
        G.state = 2;
        printf("[start_at] requested: chapter %d warp %s%d map %s%d pos %s char %d, %d bit/act writes (%s)\n", G.chapter,
               G.haveWarp ? "" : "(none)", G.haveWarp ? G.warp : 0, G.haveMap ? "" : "(any)", G.haveMap ? G.map : 0,
               G.havePos ? "raw" : "-", G.ch, G.nbits, SA_FILE);
        if (!G.haveWarp && !G.havePos && G.chapter < 2)
            printf("[start_at] no warp/pos/chapter: this is a plain new game (the prologue) -- nothing to skip\n");
    }
    if (G.state == 2 && titleReady) return 1;
    return 0;
}

/* Write a game bit into the WORK image, replicating mainSetBits (gameloop.c) for the saved banks only. */
static int sa_setbit(u8* W, int id, int value, const char* why)
{
    static const int base[4] = { 0xef0, 0x564, 0x24, 0x5d8 }, lim[4] = { 0x80, 0x74, 0x144, 0xac };
    SaGameBitDef d; int bank, i, end; unsigned bit = 1;
    id &= 0xfff;
    if (!gGameBitTable || id < 0 || id >= gGameBitCount) { printf("[start_at] %s: bit %d is outside the bit table (%d); skipped\n", why, id, gGameBitCount); return 0; }
    d = gGameBitTable[id]; bank = d.flags >> 6;
    if (bank == 0) { printf("[start_at] %s: bit %d is in volatile bank 0 (zeroed by every load); skipped\n", why, id); return 0; }
    end = d.firstBit + (d.flags & 0x1f) + 1;
    for (i = d.firstBit; i < end; i++) {
        int byteIdx = i >> 3, mask = 1 << (i & 7);
        if (byteIdx >= lim[bank]) break;
        if (value & bit) W[base[bank] + byteIdx] |= mask; else W[base[bank] + byteIdx] &= ~mask;
        bit <<= 1;
    }
    return 1;
}

/* Build the save image and edit it. 1 = ready for the title's own exit path; 0 = leave the boot as it was. */
int stfx_start_at_apply(void)
{
    u8* W; int c, i, okbits = 0, layerIdx, mapId;
    float x, y, z; int layer, angle; SaWarp wd;
    G.state = 3;
    gplayNewGame("FOX", -1);                                   /* the file-select's own new game, with no card write */
    W = gSaveGameWorkBuffer;
    if (!W) { printf("[start_at] gSaveGameWorkBuffer is NULL: abandoned, normal boot\n"); return 0; }
    if (G.chapter >= 2 && G.chapter <= 5) {
        char path[40]; int sz = 0; u8* d;
        snprintf(path, sizeof path, "/savegame/save%d.bin", G.chapter);
        d = (u8*)loadFileByPath(path, &sz, 0);
        if (d && sz >= SG_ACTIVE) {
            memcpy(W, d, SG_ACTIVE); stfx_swap_savegame_image(W);
            printf("[start_at] chapter %d: %s loaded (%d bytes) and byte-swapped into the work image\n", G.chapter, path, sz);
        } else {
            printf("[start_at] chapter %d: %s NOT loaded (%p, %d bytes): continuing with a new game's state\n", G.chapter, path, (void*)d, sz);
        }
#if !defined(VERSION_GSAE01) && !defined(VERSION_GSAJ01)
        if (d) mm_free(d);
#endif
    } else if (G.chapter != -1) {
        printf("[start_at] chapter %d is not 2..5 (0/1 are the new-game starts the file-select handles itself); ignored\n", G.chapter);
    }
    if (G.ch >= 0) W[SG_CURCHAR] = (u8)G.ch;
    c = W[SG_CURCHAR] & 1;

    /* position */
    x = ((float*)(W + SG_CHARPOS + c * 16))[0]; y = ((float*)(W + SG_CHARPOS + c * 16))[1]; z = ((float*)(W + SG_CHARPOS + c * 16))[2];
    layer = ((signed char*)(W + SG_CHARPOS + c * 16))[13]; angle = ((signed char*)(W + SG_CHARPOS + c * 16))[12];
    if (G.haveWarp) {
        if (G.warp < 0 || G.warp > 127) { printf("[start_at] warp %d is outside WARPTAB.bin (0..127): abandoned, normal boot\n", G.warp); return 0; }
        getTabEntry(&wd, MLDF_WARPTAB, G.warp << 4, 16);
        if (wd.x == 0.0f && wd.z == 0.0f) printf("[start_at] WARNING warp %d is an EMPTY entry (0,0,0): the game will try to load whatever holds cell (0,0)\n", G.warp);
        x = wd.x; y = wd.y; z = wd.z; layer = wd.layer; angle = (signed char)wd.angle;
        printf("[start_at] warp %d = (%.1f, %.1f, %.1f) layer %d heading %d\n", G.warp, x, y, z, layer, angle);
    } else if (G.havePos) {
        x = G.x; y = G.y; z = G.z; layer = G.layer; angle = (signed char)G.angle;
    }
    if (G.haveWarp || G.havePos) {                                  /* else the starting save's own position stays */
        ((float*)(W + SG_CHARPOS + c * 16))[0] = x; ((float*)(W + SG_CHARPOS + c * 16))[1] = y; ((float*)(W + SG_CHARPOS + c * 16))[2] = z;
        ((signed char*)(W + SG_CHARPOS + c * 16))[12] = (signed char)angle;
        ((signed char*)(W + SG_CHARPOS + c * 16))[13] = (signed char)layer;
        ((signed char*)(W + SG_CHARPOS + c * 16))[14] = -1;          /* mapDataFileId: none, as gplayNewGame and warps leave it */
    }
    W[SG_NEWFILE] = 1;                                              /* a used file; slot 0; none of the 0x80/0x60 file-select bits */

    /* which map will that be? (the game decides by coordinates; this is the same call mapSetup makes) */
    for (layerIdx = 0; layerIdx < 5; layerIdx++) if (gMapLayerOffsets[layerIdx] == layer) break;
    mapId = layerIdx < 5 ? mapCoordsToId((int)floorf(x / 640.0f), (int)floorf(z / 640.0f), layerIdx) : -1;
    G.appliedX = (int)floorf(x / 640.0f); G.appliedZ = (int)floorf(z / 640.0f); G.appliedLayer = layer;
    printf("[start_at] character %d at (%.1f, %.1f, %.1f) layer %d heading %d -> cell (%d,%d) -> map %d%s\n", c, x, y, z, layer, angle,
           G.appliedX, G.appliedZ, mapId, mapId < 0 ? "  (NO MAP HOLDS THIS POINT)" : "");
    if (G.haveMap && G.map != mapId) printf("[start_at] WARNING requested map %d but the point resolves to map %d; going with the point\n", G.map, mapId);

    for (i = 0; i < G.nbits; i++) {
        int id = G.bits[i].id;
        if (G.bits[i].isAct) {
            if (id < 0 || id >= 0x50 || gSaveGameMapActBits[id] == 0) { printf("[start_at] act %d: no act bit for map %d (>=0x50 maps need the extended LUT); skipped\n", G.bits[i].val, id); continue; }
            id = gSaveGameMapActBits[id];
        }
        okbits += sa_setbit(W, id, G.bits[i].val, G.bits[i].isAct ? "act" : "bit");
    }
    if (G.nbits) printf("[start_at] %d of %d bit/act writes applied to the save image\n", okbits, G.nbits);
    printf("[start_at] NOT REPLAYED: cutscene-set game bits, per-map object state, inventory/health beyond a new game, acts other than 1, Tricky, time of day. List what a place needs with bits=/act=.\n");
    printf("[start_at] handing over to the title's own exit path (titleDoLoadSave -> gotoSavegame -> loadMapForCurrentSaveGame)\n");
    return 1;
}

/* Called from vt_game.c's status block (weak): what the game DID, once the player object exists. */
#include "game/objects/object.h"
extern GameObject* Obj_GetPlayerObject(void);
void vt_print_start_at(void);
void vt_print_start_at(void)
{
    GameObject* p; int layerIdx, id; char name[32]; u8 rec[32];
    if (!G.report && (G.state != 3 || G.reported >= 3)) return;
    p = Obj_GetPlayerObject();
    if (!p) { if (G.state == 3) printf("[start_at] after the hand-over: no player object yet\n"); return; }
    G.reported++;
    for (layerIdx = 0; layerIdx < 5; layerIdx++) if (gMapLayerOffsets[layerIdx] == getCurMapLayer()) break;
    id = layerIdx < 5 ? mapCoordsToId((int)floorf(p->anim.localPosX / 640.0f), (int)floorf(p->anim.localPosZ / 640.0f), layerIdx) : -1;
    name[0] = 0;
    if (id >= 0) { getTabEntry(rec, MLDF_MAPINFO, id << 5, 32); memcpy(name, rec, 28); name[28] = 0; }
    printf("[start_at] %s: player at (%.1f, %.1f, %.1f) layer %d -> map %d \"%s\" (asked cell (%d,%d) layer %d)\n",
           G.state == 3 && G.reported <= 3 ? "game did" : "now", p->anim.localPosX, p->anim.localPosY, p->anim.localPosZ, (int)getCurMapLayer(), id, name, G.appliedX, G.appliedZ, G.appliedLayer);
}

/* ---- developer SEQUENCE SKIP: ux0:data/stairfax/seq_skip.txt starting with '1' (read once). Hook in ObjSeq_update (2.c). ----
 * While L+R+SELECT are held (raw pad: the game's reads are zero during a sequence), every sequence update asks for the sequence
 * system's own skip-to-end (slot result 2). Cost when the marker is absent: a load and a branch. */
#ifdef __vita__
#include <psp2/ctrl.h>
#endif
int stfx_seq_skip_requested(void)
{
    static int on = -1, was = 0;
#ifdef __vita__
    SceCtrlData c; int held;
    if (on < 0) {
        FILE* f = fopen("ux0:data/stairfax/seq_skip.txt", "r"); int ch = f ? fgetc(f) : 0; if (f) fclose(f);
        on = (ch == '1');
        printf("[seq_skip] developer sequence skip %s (L+R+SELECT held = end the running sequence via its skip-to-end path)\n", on ? "ON" : "off");
    }
    if (!on) return 0;
    memset(&c, 0, sizeof c);
    held = sceCtrlPeekBufferPositive(0, &c, 1) > 0 && (c.buttons & (SCE_CTRL_LTRIGGER | SCE_CTRL_RTRIGGER | SCE_CTRL_SELECT)) == (SCE_CTRL_LTRIGGER | SCE_CTRL_RTRIGGER | SCE_CTRL_SELECT);
    if (held && !was) printf("[seq_skip] chord held: sequences skip to their end\n");
    was = held;
    return held;
#else
    (void)was; on = 0;
    return 0;
#endif
}
