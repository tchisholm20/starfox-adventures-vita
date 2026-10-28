// stfx_state_dump.c - the PORT half of the state oracle (run 252, 2026-10-13).
// Built with the game units' flags by compile_all.sh (globbed from src/port/src/hal).
//
// host/engine/state_oracle.py drives RETAIL under Dolphin to a named moment and writes every game bit and every loaded
// object as `stfx-state-v1` JSON (host/engine/state_oracle_dump_format.txt); state_oracle_diff.py compares two dumps.
// This file writes the same record from the port, at the same kind of moment: the N-th gameLoop() call after
// loadMapForCurrentSaveGame(). Because the source is retail's, a wrong story state is a wrong INPUT to correct logic,
// and the diff names it without anybody playing (runs 246-251 found one by hand: docs/47).
//
// ux0:data/stairfax/state_dump.txt  one integer N. Absent or 0 = never dump. The record is written once per map load to
//                                   ux0:data/stairfax_state.json (beside the game log, pulled the same way).
// Fields are read at the RAW offsets the retail reader uses (GameObject is the same layout on both: 32-bit pointers,
// and the headers STATIC_ASSERT it), so a layout difference shows up as a difference instead of hiding behind a name.
#include <stdio.h>
#include <string.h>
#include "types.h"
#include "game/objects/object.h"

#include "main/obj_list.h"
#include "sys/objects.h"
extern u32 mainGetBit(int gameBit);
extern s16 gGameBitCount;

#define SD_CFG "ux0:data/stairfax/state_dump.txt"
#define SD_OUT "ux0:data/stairfax_state.json"

static int sWanted = -1;   /* -1 = config not read yet */
static int sArmed, sCount;

static void sd_object(FILE* f, const u8* o, int idx) {
    const u8* placement = *(const u8* const*)(o + 0x4C);
    const u8* extra = *(const u8* const*)(o + 0xB8);
    fprintf(f, "{\"idx\":%d,\"addr\":%u,\"classId\":%d,\"romDefNo\":%d,\"flags\":%u,\"x\":%.4f,\"y\":%.4f,\"z\":%.4f,\"ident\":", idx, (unsigned)(size_t)o,
            (int)*(const s16*)(o + 0x44), (int)*(const s16*)(o + 0x46), (unsigned)*(const u16*)(o + 0xB0), (double)*(const f32*)(o + 0x18),
            (double)*(const f32*)(o + 0x1C), (double)*(const f32*)(o + 0x20));
    if (placement) fprintf(f, "%d", (int)*(const s32*)(placement + 0x14));
    else fprintf(f, "null");
    /* hp: retail reads these two words for every object with an extra block (neighbouring bytes for a non-enemy). Only the
       Baddie family's extra (0x370 bytes) is known to reach 0x2B4 here, so the port reports it for class 28 / def 17 style
       enemies only via the size the object itself declares; the diff compares hp only where both sides report it. */
    if (extra) {
        ObjectInterfaceHandle h = ((const GameObject*)o)->anim.dll;   /* descriptor slots from slot02: [1] init ... [7] getExtraSize */
        int szFn = h ? ((int*)*h)[7] : 0;
        int sz = (szFn != 0 && szFn != -1) ? ((int (*)(void))szFn)() : 0;
        if (sz >= 0x2B4) fprintf(f, ",\"hpCur\":%u,\"hpMax\":%u", (unsigned)*(const u16*)(extra + 0x2B0), (unsigned)*(const u16*)(extra + 0x2B2));
    }
    fprintf(f, "}");
}

static void sd_write(void) {
    FILE* f = fopen(SD_OUT, "w");
    int first, count, i, n = 0;
    GameObject** objs;
    const u8* player;
    if (!f) { fprintf(stderr, "[state] could not open %s\n", SD_OUT); return; }
    fprintf(f, "{\"format\":\"stfx-state-v1\",\"source\":\"port\",\"moment\":\"port: gameLoop call %d after loadMapForCurrentSaveGame\",\"bitCount\":%d,\"bits\":{",
            sCount, (int)gGameBitCount);
    for (i = 0; i < gGameBitCount; i++) {
        u32 v;
        if (i == 0x95 || i == 0x96) continue;   /* mainGetBit answers these two with constants; retail's table decode does not */
        v = mainGetBit(i);
        if (v) fprintf(f, "%s\"%d\":%u", n++ ? "," : "", i, (unsigned)v);
    }
    fprintf(f, "},\"player\":");
    player = (const u8*)Obj_GetPlayerObject();
    if (player) sd_object(f, player, -1); else fprintf(f, "null");
    objs = ObjList_GetObjects(&first, &count);
    fprintf(f, ",\"objectCount\":%d,\"objects\":[", count);
    for (i = 0; i < count; i++) {
        if (i) fprintf(f, ",");
        if (objs[i]) sd_object(f, (const u8*)objs[i], i); else fprintf(f, "null");
    }
    fprintf(f, "]}\n");
    fclose(f);
    fprintf(stderr, "[state] wrote %s: %d nonzero bits of %d, %d objects (list starts at %d), gameLoop call %d after the map load\n", SD_OUT, n, (int)gGameBitCount,
            count, first, sCount);
}

/* called at the top of loadMapForCurrentSaveGame (dlls/engine/23) */
void stfx_state_map_loaded(void) {
    if (sWanted < 0) {
        FILE* f = fopen(SD_CFG, "r");
        sWanted = 0;
        if (f) { if (fscanf(f, "%d", &sWanted) != 1) sWanted = 0; fclose(f); }
        fprintf(stderr, "[state] dump %s (%s: %d)\n", sWanted > 0 ? "ARMED at each save-game map load" : "off", SD_CFG, sWanted);
    }
    sArmed = sWanted > 0;
    sCount = 0;
}

/* called at the top of gameLoop (main/gameloop.c) */
void stfx_state_tick(void) {
    if (!sArmed) return;
    if (++sCount < sWanted + 1) return;
    sArmed = 0;
    sd_write();
}
