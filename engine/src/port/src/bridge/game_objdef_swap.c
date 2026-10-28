// game_objdef_swap.c - byte-swap an OBJECTS.bin object definition (ObjDef) from
// big-endian to host order, so the recompiled loadObjectFile relocates its offset
// fields correctly and loadCharacter reads its scalars (dllId/flags/modelFileIds/...)
// as real values. Called from the port fileLoadToBufferOffset right after a def is
// loaded (id MLDF_FILEID_OBJECTS_BIN), before loadObjectFile relocates it.
// See include/main/objanim_internal.h (ObjDef), port/byteswap.h, [[port-object-system]].

#include "main/objanim_internal.h"
#include "port/byteswap.h"

#include <stdio.h>

/* Side-table walks are bounded by the def's length (the OBJECTS.bin slice size). A table whose extent would
   leave the def is refused with one stderr line and left raw -- never walked off the end. */
static int objdefRange(int defNo, int len, const char* what, unsigned off, unsigned bytes) {
    if (off > (unsigned)len || bytes > (unsigned)len - off) {
        fprintf(stderr, "[endian] objdef %d: %s out of range -- left raw\n", defNo, what);
        return 0;
    }
    return 1;
}

/* An s16 triple table {moveId, byteOffset, byteCount} ended by moveId == -1 (ObjAnim_LoadMoveEvents, objGetWeaponDa,
   ObjHitReact_LoadMoveEntries). Walks BEFORE swapping so a missing sentinel leaves the table untouched. */
static void swapTripleTable(unsigned char* d, int len, int defNo, const char* what, unsigned off) {
    unsigned p = off; int n = 0;
    if (!off) return;
    for (;;) {
        if (!objdefRange(defNo, len, what, p, 2)) return;
        if (d[p] == 0xff && d[p + 1] == 0xff) break;
        if (!objdefRange(defNo, len, what, p, 6)) return;
        p += 6; ++n;
    }
    for (int i = 0; i < n; ++i) { beFix16(d + off + i * 6); beFix16(d + off + i * 6 + 2); beFix16(d + off + i * 6 + 4); }
    beFix16(d + p);   /* the sentinel, so the s16 compare sees -1 either way */
}

void bswapObjDefN(void* p, int len, int defNo);
void bswapObjDef(void* p) { bswapObjDefN(p, -1, -1); }

void bswapObjDefN(void* p, int len, int defNo) {
    ObjDef* d = (ObjDef*)p;
    // scalars loadCharacter/objSetupObject read
    BEF32(d->shadowScaleBase); BEF32(d->rootMotionScaleBase);
    BE32(d->flags);
    BE16(d->shadowType); BE16(d->shadowTextureId); BE16(d->hitboxFlags);
    BE16(d->dllId); BE16(d->category);
    BE16(d->primaryCapsuleOffsetA); BE16(d->primaryCapsuleOffsetB);
    BE16(d->secondaryCapsuleOffsetA); BE16(d->secondaryCapsuleOffsetB);
    BE16(d->mapLoadObjectId); BE16(d->npcDialogueTextId);
    for (int i = 0; i < 4; ++i) BE16(d->helpTextIds[i]);
    BE16(d->avoidMinDistance); BE16(d->avoidMoveDistance); BEF32(d->shadowModelScaleBase);
    // the exact offset fields loadObjectFile relocates (must be host order first)
    BE32(d->modelFileIds); BE32(d->textureSlotDefs); BE32(d->jointData);
    BE32(d->extraSetupData); BE32(d->sequenceMap); BE32(d->eventMoveTable);
    BE32(d->hitReactMoveTable); BE32(d->weaponDaTable); BE32(d->attachPoints);
    BE32(d->hitVolumes);
    // modelFileIds[] (offset now host order): swap its modelCount s32 entries
    unsigned mfOff = *(unsigned*)&d->modelFileIds;
    if (mfOff) { int* mf = (int*)((unsigned char*)d + mfOff);
        for (int i = 0; i < d->modelCount; ++i) beFix32(&mf[i]); }
    // sequenceMap[] (s16, sequenceCount entries): local -> global OBJSEQ index (2.c ObjSeq_start).
    // Raw, a title-screen door ran a byte-reversed sequence that cast the absent player (Vita
    // runs 40/41: playerStatusIsPositive(NULL)). On the Rev 1 disc 1306/1306 entries in 413 defs
    // index OBJSEQ (< 1488) read big-endian; 91 read raw.
    unsigned smOff = *(unsigned*)&d->sequenceMap;
    if (smOff) { short* sm = (short*)((unsigned char*)d + smOff);
        for (int i = 0; i < d->sequenceCount; ++i) beFix16(&sm[i]); }
    // hitVolumes[] (ObjDefHitVolume, 0x18 each, hitVolumeCount entries): six s16 (jointOffset XYZ, pos XYZ)
    // then bytes. Read natively by the hit-volume transform setup; raw, every offset is byte-reversed.
    unsigned hvOff = *(unsigned*)&d->hitVolumes;
    if (hvOff) { unsigned char* hv = (unsigned char*)d + hvOff;
        for (int i = 0; i < d->hitVolumeCount; ++i) for (int k = 0; k < 6; ++k) beFix16(hv + i * 0x18 + k * 2); }
    if (len < 0) return;   // unbounded legacy entry (bswapObjDef): side tables are NOT walked
    {   unsigned char* b = (unsigned char*)d;
        // s16 triple tables (docs/47 audit 2026-10-12): objects' move-event / hit-react / weapon-data indexes.
        swapTripleTable(b, len, defNo, "eventMoveTable", *(unsigned*)&d->eventMoveTable);
        swapTripleTable(b, len, defNo, "hitReactMoveTable", *(unsigned*)&d->hitReactMoveTable);
        swapTripleTable(b, len, defNo, "weaponDaTable", *(unsigned*)&d->weaponDaTable);
        // attachPoints: ObjAttachPoint 0x18 each = {f32 pos[3]@0, s16 rot[3]@0xC, s8 joints[6]@0x12} x attachPointCount.
        unsigned ap = *(unsigned*)&d->attachPoints;
        if (d->attachPointCount && ap && objdefRange(defNo, len, "attachPoints", ap, d->attachPointCount * 0x18u)) {
            for (int i = 0; i < d->attachPointCount; ++i) {
                unsigned char* a = b + ap + i * 0x18;
                beFix32(a); beFix32(a + 4); beFix32(a + 8); beFix16(a + 0xC); beFix16(a + 0xE); beFix16(a + 0x10);
            }
        }
        // extraSetupData: only DLL 237 (Collectible, 0xED) overlays a multi-byte field: CollectibleModelSetup.pickupCategory
        // s16 @2 (237.c:173). DLL 255 reads an s8 @0xB; DLLs 198/262/280 have the block but no reader. Keyed by dllId.
        unsigned ex = *(unsigned*)&d->extraSetupData;
        if (ex && d->dllId == 0xED && objdefRange(defNo, len, "extraSetupData", ex, 4)) beFix16(b + ex + 2);
    }
}
