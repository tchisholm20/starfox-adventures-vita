/* gxs_keys.h - the SHADER KEY LOG / HARVEST record format (2026-11-03), host-compilable. Included by rhi_gxm_shader.c in its PURE part, so the host test
 * (vita/host/tests/shader_keys_test.c, -DGXM_SHADER_HOST) runs the SAME serialise / parse / rebuild / hash code the console runs.
 *
 * WHY: a key the pack and the GXP cache do not hold is compiled on the console (~2 s, up to 10 s each). The emulator (which never compiles) LOGS every such key
 * (ux0:data/stairfax/shader_keys.bin); the host merges the logs and subtracts what ships (shader_harvest.py); the console replays the list through its
 * background compiler (ux0:data/stairfax/harvest.bin), which saves each result to the GXP cache.
 *
 * FILE (little-endian, both shader_keys.bin and harvest.bin):
 *   header, 16 bytes:  char magic[8] = "STFXKEY1"; u32 version = 1; u32 GXS_SHC_VERSION (the generator / pack version the keys were made under)
 *   record:            u8 kind ('t' TEV exact, 'f' TEV fast, 'g' GX exact, 'h' GX fast); u8 family (0 TEV, 1 GX = GXM_PK_*); u8 fast (gxs_make's `fast`: 0 exact,
 *                      1 shipping fast, 1 | vbits << 1 a generator variant); u8 0; u32 h (the program hash gxs_make was called with); u32 h2 (gxs_key_h2);
 *                      u16 n1; u16 n2; then k1 (n1 bytes) and k2 (n2 bytes), the pair padded to 4 together.
 *   TEV record: k1 = the canonical TevKey's first tev_key_len bytes, n2 = 0.   GX record: k1 = the VsKey's first tev_vs_key_len bytes, k2 = the TevKey's first
 *   tev_key_len bytes.  These are exactly the bytes the GXP-cache file stores (gxs_save_gxpc), so a record names a cache file: <kind><h:08x>.gxp.
 *
 * ROUND TRIP IS EXACT because identity is defined by the first tev_key_len / tev_vs_key_len bytes ONLY (hash, h2, cache validity all read no more): a rebuilt key is
 * the zeroed struct with those bytes copied in, and gxs_keys_rebuild REFUSES unless the length function of the rebuilt key gives back n again (so a key whose
 * length was decided by a byte beyond n cannot slip through). h and h2 are then RECOMPUTED here and must equal the record's (gxs_keys_verify). */
#ifndef GXS_KEYS_H
#define GXS_KEYS_H

#define GXS_KEYS_VERSION 1u
#define GXS_KEYS_HDR     16u
#define GXS_KEYS_RHDR    16u
#define GXS_KF_TEV       0       /* = GXM_PK_TEV */
#define GXS_KF_GX        1       /* = GXM_PK_GX  */

typedef struct { uint8_t kind, family, fast; uint32_t h, h2; uint16_t n1, n2; const uint8_t *k1, *k2; } GxsKeyRec;

GXS_PURE int gxs_keys_kind(int gx, int fast) { return gx ? (fast ? 'h' : 'g') : (fast ? 'f' : 't'); }
GXS_PURE uint32_t gxs_keys_rec_bytes(unsigned n1, unsigned n2) { return GXS_KEYS_RHDR + gxs_pad4(n1 + n2); }
GXS_PURE void gxs_keys_header(uint8_t out[GXS_KEYS_HDR])
{
    memcpy(out, "STFXKEY1", 8);
    const uint32_t v = GXS_KEYS_VERSION, s = GXS_SHC_VERSION;
    memcpy(out + 8, &v, 4); memcpy(out + 12, &s, 4);
}
/* 0 ok; 1 short; 2 bad magic; 3 unknown file version; 4 made under another GXS_SHC_VERSION */
GXS_PURE int gxs_keys_header_check(const uint8_t *f, uint32_t n)
{
    if (n < GXS_KEYS_HDR) return 1;
    if (memcmp(f, "STFXKEY1", 8)) return 2;
    if (gxs_rd32(f + 8) != GXS_KEYS_VERSION) return 3;
    if (gxs_rd32(f + 12) != GXS_SHC_VERSION) return 4;
    return 0;
}
/* write one record at `out` (gxs_keys_rec_bytes(n1, n2) bytes; the padding is zeroed). Returns the byte count. */
GXS_PURE uint32_t gxs_keys_put(uint8_t *out, int gx, int fast, uint32_t h, uint32_t h2, const void *k1, unsigned n1, const void *k2, unsigned n2)
{
    const uint32_t total = gxs_keys_rec_bytes(n1, n2);
    memset(out, 0, total);
    out[0] = (uint8_t) gxs_keys_kind(gx, fast); out[1] = (uint8_t) (gx ? GXS_KF_GX : GXS_KF_TEV); out[2] = (uint8_t) fast; out[3] = 0;
    memcpy(out + 4, &h, 4); memcpy(out + 8, &h2, 4);
    const uint16_t a = (uint16_t) n1, b = (uint16_t) n2;
    memcpy(out + 12, &a, 2); memcpy(out + 14, &b, 2);
    memcpy(out + GXS_KEYS_RHDR, k1, n1);
    if (n2) memcpy(out + GXS_KEYS_RHDR + n1, k2, n2);
    return total;
}
/* parse the record at `f` (`avail` bytes left). Returns the bytes it occupies, 0 if short / malformed (a key longer than the structs, a set pad byte). r points INTO f. */
GXS_PURE uint32_t gxs_keys_get(const uint8_t *f, uint32_t avail, GxsKeyRec *r)
{
    if (avail < GXS_KEYS_RHDR) return 0;
    uint16_t a, b; memcpy(&a, f + 12, 2); memcpy(&b, f + 14, 2);
    if (a > sizeof(VsKey) + sizeof(TevKey) || b > sizeof(TevKey)) return 0;
    const uint32_t total = gxs_keys_rec_bytes(a, b);
    if (avail < total || f[3] != 0) return 0;
    r->kind = f[0]; r->family = f[1]; r->fast = f[2]; r->h = gxs_rd32(f + 4); r->h2 = gxs_rd32(f + 8); r->n1 = a; r->n2 = b;
    r->k1 = f + GXS_KEYS_RHDR; r->k2 = f + GXS_KEYS_RHDR + a;
    return total;
}
/* rebuild the keys from a record: 0 ok; 1 shape (kind / family / fast / lengths disagree); 2 a length function does not give n back. `vk` is zeroed for a TEV record. */
GXS_PURE int gxs_keys_rebuild(const GxsKeyRec *r, VsKey *vk, TevKey *ck)
{
    const int gx = r->kind == 'g' || r->kind == 'h', tev = r->kind == 't' || r->kind == 'f';
    if (!gx && !tev) return 1;
    if (r->family != (gx ? GXS_KF_GX : GXS_KF_TEV)) return 1;
    if ((r->kind == 'f' || r->kind == 'h') != (r->fast != 0)) return 1;
    memset(vk, 0, sizeof *vk); memset(ck, 0, sizeof *ck);
    if (gx) {
        if (r->n1 > sizeof(VsKey) || r->n2 > sizeof(TevKey)) return 1;
        memcpy(vk, r->k1, r->n1); memcpy(ck, r->k2, r->n2);
        if (tev_vs_key_len(vk) != r->n1 || tev_key_len(ck) != r->n2) return 2;
    } else {
        if (r->n2 || r->n1 > sizeof(TevKey)) return 1;
        memcpy(ck, r->k1, r->n1);
        if (tev_key_len(ck) != r->n1) return 2;
    }
    return 0;
}
/* the program hash gxs_make is called with, from the CANONICAL keys (gxs_tev_identity_v / gxs_gx_identity_v: th = td_hash of the canonical key, salted when fast, salted by the variant) */
GXS_PURE uint32_t gxs_keys_hash(int gx, int fast, const VsKey *vk, const TevKey *ck)
{
    uint32_t h = td_hash(ck) ^ (fast ? GXS_FAST_SALT : 0u) ^ gxs_var_salt(gxs_fast_vbits(fast));
    if (gx) h ^= tev_vs_key_hash(vk) * 0x9E3779B9u;
    return h;
}
/* rebuild + recompute: 0 ok (vk / ck valid); 1 / 2 as gxs_keys_rebuild; 3 the recomputed h or h2 differs from the record's */
GXS_PURE int gxs_keys_verify(const GxsKeyRec *r, VsKey *vk, TevKey *ck)
{
    const int rc = gxs_keys_rebuild(r, vk, ck);
    if (rc) return rc;
    const int gx = r->family == GXS_KF_GX;
    if (gxs_keys_hash(gx, r->fast, vk, ck) != r->h) return 3;
    if (gxs_key_h2(gx ? vk : NULL, ck) != r->h2) return 3;
    return 0;
}
#endif
