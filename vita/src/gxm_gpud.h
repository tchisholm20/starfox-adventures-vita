/* gxm_gpud.h -- the in-boot GPU-cost A/B arms ("gpud"): the PURE part (marker parser + arm schedule), no SDK types, so a host test compiles it with the
 * same gcc as the other gxm_*_test.c files (host/tests/gxm_gpud_parse_test.c).  The state, the arms and the log live in rhi_gxm_target.c / _draw.c / _shader.c.
 *
 * Marker ux0:data/stairfax/gpud.txt (read once at init):
 *   absent / "0"           OFF: the build behaves as before (one branch on g_gpud_on per draw)
 *   "ab <period>"          arms cycle  A F A H A G A L A X  (10 windows per cycle, the SHADER-VARIANT arms), <period> frames each (rounded UP to a multiple of 60), A first
 *   "ab-old <period>"      the previous schedule  A B A C A D A E  (8 windows per cycle) -- kept so the cost-removal arms can still be run in one boot
 *   "A".."E","F","G","H","L","X" (any case)   that single arm for the whole boot
 *   anything else          OFF, and the boot line says it was garbage
 * Arms (draws into M only: drawTev / drawTevGx, C6 runs included): A baseline | B every fragment texture unit bound to one 4x4 grey texture |
 * C constant-colour fragment program, no texture read / varying / clip / fog | D fog type forced to 0 | E alpha compare forced to ALWAYS |
 * SHADER-GENERATOR VARIANTS (engine tev_gen.c TEVV_*; the arm sets the variant of every TEV / TEVGX program lookup, through the same key-copy point as D / E):
 *   F exact -> fast for keys the fast variant refuses ONLY for its error bound | H fp16 stage code + colour varyings | G fog tail in half with one vector lerp |
 *   L plain lerp weight (no GX c + (c >> 7) term) | X = H + G + L together (the candidate default). */
#ifndef GXM_GPUD_H
#define GXM_GPUD_H
#include <stdint.h>

enum { GPUD_OFF = 0, GPUD_AB = 1, GPUD_SINGLE = 2, GPUD_ABOLD = 3 };
typedef struct { int mode; int period; char arm; int garbage; } GpudCfg;   /* period: frames per window (AB / ABOLD), or the log period (SINGLE: 600) */

/* TEVV_* of engine/src/port/include/port/gx_tev_key.h, repeated here because this header is SDK-free; rhi_gxm_target.c holds them equal with _Static_assert */
#define GPUD_V_H 1u
#define GPUD_V_G 2u
#define GPUD_V_L 4u
#define GPUD_V_F 8u

#define GPUD_SEQ_N 10           /* windows per cycle of the shader-variant schedule */
#define GPUD_SEQ_OLD_N 8
static inline int gxm_gpud_seq_n(int mode) { return mode == GPUD_ABOLD ? GPUD_SEQ_OLD_N : GPUD_SEQ_N; }
static inline char gxm_gpud_arm_of_window(int mode, unsigned w)
{
    static const char sn[GPUD_SEQ_N + 1] = "AFAHAGALAX", so[GPUD_SEQ_OLD_N + 1] = "ABACADAE";
    return mode == GPUD_ABOLD ? so[w % GPUD_SEQ_OLD_N] : sn[w % GPUD_SEQ_N];
}
static inline unsigned gxm_gpud_cycle_of_window(int mode, unsigned w) { return w / (unsigned) gxm_gpud_seq_n(mode); }
/* the generator variant bits an arm runs with (0: the arm is not a variant arm) */
static inline unsigned gxm_gpud_variant_of_arm(char a)
{
    switch (a) { case 'F': return GPUD_V_F; case 'H': return GPUD_V_H; case 'G': return GPUD_V_G; case 'L': return GPUD_V_L; case 'X': return GPUD_V_H | GPUD_V_G;   /* 2026-11-02: L failed the host error gate (max 3 codes): X = the candidate default H|G; L stays a cost-only arm */ default: return 0u; }
}
static inline int gxm_gpud_is_arm_letter(char c) { return (c >= 'A' && c <= 'H') || c == 'L' || c == 'X'; }

static inline int gxm_gpud_ws(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

/* b/len: the file's bytes (not necessarily NUL-terminated); len <= 0: file absent or empty */
static inline GpudCfg gxm_gpud_parse(const char *b, int len)
{
    GpudCfg c; c.mode = GPUD_OFF; c.period = 0; c.arm = 'A'; c.garbage = 0;
    if (!b || len <= 0) return c;
    int i = 0;
    while (i < len && gxm_gpud_ws(b[i])) ++i;
    if (i >= len || b[i] == 0) return c;                                   /* only blanks: as absent */
    const char c0 = b[i];
    int e = i + 1; while (e < len && b[e] && gxm_gpud_ws(b[e])) ++e;      /* e: first non-blank after the first char */
    const int alone = e >= len || !b[e];                                   /* nothing follows the first char */
    if (c0 == '0' && alone) return c;
    if ((c0 == 'a' || c0 == 'A') && i + 1 < len && (b[i + 1] == 'b' || b[i + 1] == 'B')) {   /* "ab <period>" / "ab-old <period>" */
        int j = i + 2, old = 0;
        if (j + 3 < len && b[j] == '-' && (b[j + 1] == 'o' || b[j + 1] == 'O') && (b[j + 2] == 'l' || b[j + 2] == 'L') && (b[j + 3] == 'd' || b[j + 3] == 'D')) { old = 1; j += 4; }
        while (j < len && b[j] && gxm_gpud_ws(b[j])) ++j;
        long n = 0; int digits = 0;
        while (j < len && b[j] >= '0' && b[j] <= '9') { if (n < 10000000L) n = n * 10 + (b[j] - '0'); ++digits; ++j; }
        if (digits && n > 0 && (j >= len || !b[j] || gxm_gpud_ws(b[j]))) { c.mode = old ? GPUD_ABOLD : GPUD_AB; c.period = (int) ((n + 59) / 60 * 60); return c; }
        c.garbage = 1; return c;
    }
    if (alone) {
        const char u = (c0 >= 'a' && c0 <= 'z') ? (char) (c0 - 'a' + 'A') : c0;
        if (gxm_gpud_is_arm_letter(u)) { c.mode = GPUD_SINGLE; c.arm = u; c.period = 600; return c; }
    }
    c.garbage = 1;
    return c;
}
#endif
