/* tev_dedupe.h -- one GL program per distinct fragment PROGRAM TEXT, not per distinct TevKey struct (2026-10-12, docs/47 "Run 233").
 * No GL, no OS: the device (rhi_vita_tev.inc) and the host test (host/tests/tev_canon_test.c) compile this same header.
 *
 * WHY. Run 233 (play session, 5 MB log) built 1,091 pair programs over 829 distinct TEV key hashes while its off-thread compiler
 * saw only 125 distinct fragment sources (+64 vertex): 696 pairs were linked from shader objects, over 568 TEV hashes, and the
 * shared-shader-object table (shobj, by source text) had already folded those into the 125. Only the PROGRAM was per key. 832
 * programs sat live against vitaGL's 1,024 (budget 896), 576 evictions cost 2.9 s in glDeleteProgram (~5 ms each), and 819 program
 * binaries were written. The key is a memcmp of the whole 406-byte struct, and gx_tev_build copies GX state the generator never reads
 * (konst selectors of stages that read no konst, swap rows nobody points at, an alpha compare that cannot reject, ...:
 * tev_gen.c tev_key_canon lists them).
 *
 * WHAT THIS DOES. The program tables stay keyed by a key -- but by the CANONICAL one. A raw TevKey goes through a small map (hash64 of
 * the 406 bytes -> canonical record) so tev_key_canon + tev_fast_safe run once per raw key, not per draw; the canonical record holds the
 * canonical key, its hash, and the variant (fast / exact: decided from the RAW key, because tev_fast_safe may differ between two keys
 * with one canonical form, and the variant is part of the program's identity). Nothing is shared between table entries: no refcount, no
 * second owner of a GL program. Two raw keys with one canonical form are the SAME entry, so evicting it (prog_lru.h) evicts the only
 * user, and the next draw of EITHER raw key reloads it once. Per-draw state (registers, konst, alpha refs, fog numbers, matrices,
 * textures) is supplied by the caller on every draw from the RAW key's uniforms; the only state a program keeps is its texture units
 * (set once from numTex, which is in the program text) -- docs/47 questions 3.
 *
 * Break arm: ux0:data/STFX00001/tev_dedupe_off.txt starting with 1 = canonical key == raw key (today's behaviour). */
#ifndef TEV_DEDUPE_H
#define TEV_DEDUPE_H
#include <stdint.h>
#include <string.h>
#include "port/gx_tev_key.h"

#define TD_RAW 4096   /* raw keys remembered: run 233 saw 829 TEV hashes; a full map falls back to the slow path (td_over) */
#define TD_CAN 768    /* canonical records (~416 bytes = 320 KB). Run 233 had 829 distinct raw TEV keys, so canonical <= 829 in the worst case (nothing merged) and ~150-300 expected; a full table falls back to the RAW key (below), counted */
#define TD_FAST_SALT 0x0FA57A57u

typedef struct { TevKey ck; uint32_t th; uint8_t fast; } TdCan;   /* th: tev hash of ck, UNSALTED (the number the logs print) */
typedef struct { uint32_t r1, r2; uint16_t ci; } TdRaw;           /* ci: canonical index + 1, 0 = empty */
typedef struct {
    int on;                                  /* 0: the break arm */
    TdRaw raw[TD_RAW]; TdCan can[TD_CAN];
    int nraw, ncan;
    unsigned seen;                           /* distinct raw keys that reached a lookup */
    unsigned aliased;                        /* of those: raw keys whose canonical record already existed (their program is shared) */
    unsigned over;                           /* lookups answered by the slow path because a table was full (never a wrong answer) */
    uint32_t srcs[1024][2]; int nsrcs;       /* distinct fragment source texts seen at build time (td_note_src) */
    unsigned src_total;                      /* builds noted */
    TdCan scratch;                           /* the answer when the map is off or full */
} Td;

static uint32_t td_hash(const TevKey *k)   /* the device's program hash (was rhi_vita_tev.inc tev_hash) */
{
    const uint8_t *p = (const uint8_t *) k; uint32_t h = 2166136261u ^ (uint32_t) TEVGEN_VERSION * 0x9E3779B9u;
    for (size_t i = 0, n = tev_key_len(k); i < n; ++i) h = (h ^ p[i]) * 16777619u;   /* the legacy bytes of a key without indirect stages: hashes (and saved binaries) are unchanged */
    /* 2026-10-12: a key with a COMPARE stage (op 8..15) used to be emitted as an add, so every binary saved for such a key is wrong. Its hash
       moves (only its hash: the other saved programs keep theirs), so the stale files and pack records are never found and it is compiled again. */
    if (tev_has_compare(k)) h ^= 0xC04DC04Du;
    return h;
}
static void td_raw_hash(const TevKey *k, uint32_t *a, uint32_t *b)   /* 64 bits over the raw bytes: two independent mixes */
{
    const uint8_t *p = (const uint8_t *) k; uint32_t h1 = 2166136261u, h2 = 0x7F4A7C15u;
    for (size_t i = 0; i < sizeof *k; ++i) { h1 = (h1 ^ p[i]) * 16777619u; h2 = (h2 + p[i]) * 0x85EBCA6Bu; h2 ^= h2 >> 13; }
    *a = h1; *b = h2;
}
static void td_fill(TdCan *c, const TevKey *raw, int fastOn, int on)
{
    const int fast = fastOn && tev_fast_safe(raw);
    if (on) tev_key_canon(raw, &c->ck, fast); else c->ck = *raw;
    c->th = td_hash(&c->ck); c->fast = (uint8_t) fast;
}
/* The canonical record for a raw key. The pointer is valid until the next call (table entries are stable; the scratch is not). */
static const TdCan *td_get(Td *t, const TevKey *raw, int fastOn)
{
    if (!t->on) { td_fill(&t->scratch, raw, fastOn, 0); return &t->scratch; }
    uint32_t a, b; td_raw_hash(raw, &a, &b);
    unsigned p = a & (TD_RAW - 1);
    for (int c = 0; c < TD_RAW; ++c, p = (p + 1) & (TD_RAW - 1)) {
        if (!t->raw[p].ci) break;
        if (t->raw[p].r1 == a && t->raw[p].r2 == b) return &t->can[t->raw[p].ci - 1];
    }
    TdCan tmp; td_fill(&tmp, raw, fastOn, 1);
    int ci = 0;
#ifndef TD_BREAK_NOMERGE   /* host negative control: every raw key gets its own record (no aliasing) */
    for (int i = 0; i < t->ncan; ++i)
        if (t->can[i].fast == tmp.fast && t->can[i].th == tmp.th && !memcmp(&t->can[i].ck, &tmp.ck, sizeof tmp.ck)) { ci = i + 1; break; }
#endif
    if ((!ci && t->ncan == TD_CAN) || t->nraw == TD_RAW - 1) { ++t->over; td_fill(&t->scratch, raw, fastOn, 0); return &t->scratch; }   /* the RAW key: exactly today's identity for it, never a drop */
    ++t->seen;
    if (ci) ++t->aliased; else { t->can[t->ncan++] = tmp; ci = t->ncan; }
    t->raw[p].r1 = a; t->raw[p].r2 = b; t->raw[p].ci = (uint16_t) ci; ++t->nraw;
    return &t->can[ci - 1];
}
/* a fragment source text built (or about to be loaded) for a program: how many DISTINCT texts the canonical keys really produce */
static void td_note_src(Td *t, const char *src)
{
    uint32_t h1 = 2166136261u, h2 = 0x9E3779B9u;
    for (const unsigned char *q = (const unsigned char *) src; *q; ++q) { h1 = (h1 ^ *q) * 16777619u; h2 = (h2 + *q) * 0x85EBCA6Bu; h2 ^= h2 >> 13; }
    ++t->src_total;
    for (int i = 0; i < t->nsrcs; ++i) if (t->srcs[i][0] == h1 && t->srcs[i][1] == h2) return;
    if (t->nsrcs < 1024) { t->srcs[t->nsrcs][0] = h1; t->srcs[t->nsrcs][1] = h2; ++t->nsrcs; }
}
#endif
