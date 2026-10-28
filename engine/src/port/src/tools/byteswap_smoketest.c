// byteswap_smoketest.c - verifies port/byteswap.h (the endianness facility).
// Deterministic, no deps: builds BE byte patterns and checks value-readers, in-place
// converters, field macros, and the array/record helpers used at load time.

#include "port/byteswap.h"
#include <stdio.h>
#include <string.h>

static int gFails;
static void chk(const char* what, long got, long want) {
    if (got != want) { printf("  FAIL %-26s got=%ld want=%ld\n", what, got, want); gFails++; }
    else               printf("  ok   %-26s = %ld\n", what, got);
}

// A representative BE-on-disc record with mixed field widths.
typedef struct { uint16_t a; uint8_t b; uint8_t c; uint32_t d; float e; } Rec;
static void bswapRec(void* p) { Rec* r = (Rec*)p; BE16(r->a); BE32(r->d); BEF32(r->e); }

int main(void) {
    // primitives
    chk("bswap16", stfx_bswap16(0x1234), 0x3412);
    chk("bswap32", (long)stfx_bswap32(0x11223344u), (long)0x44332211u);

    // value readers over BE bytes
    unsigned char be16[2] = { 0x00, 0x4C };            // 76
    unsigned char be32[4] = { 0x00, 0x01, 0x02, 0x03 };// 0x00010203
    chk("beRead16", beRead16(be16), 76);
    chk("beRead32", (long)beRead32(be32), 0x00010203);
    unsigned char bef[4] = { 0x3f, 0x80, 0x00, 0x00 }; // 1.0f BE
    chk("beReadF32==1.0", beReadF32(bef) == 1.0f, 1);

    // in-place converters + field macros
    uint16_t v16 = 0; unsigned char* p16 = (unsigned char*)&v16; p16[0] = 0x00; p16[1] = 0x4C;
    beFix16(&v16); chk("beFix16 -> 76", v16, 76);
    uint32_t v32; unsigned char* p32 = (unsigned char*)&v32;
    p32[0]=0x0a;p32[1]=0x0b;p32[2]=0x0c;p32[3]=0x0d; beFix32(&v32);
    chk("beFix32", (long)v32, 0x0a0b0c0d);

    // array helper
    unsigned char arr[6] = { 0x00,0x01, 0x00,0x02, 0x00,0x03 };
    beFixArray16(arr, 3);
    chk("array16[0]", ((uint16_t*)arr)[0], 1);
    chk("array16[2]", ((uint16_t*)arr)[2], 3);

    // record helper over a mixed struct (the real load-time pattern)
    Rec recs[2];
    unsigned char* r = (unsigned char*)recs;
    memset(recs, 0, sizeof recs);
    // rec0: a=0x004C(76) b=1 c=2 d=0x00010203 e=1.0f, all BE in memory
    r[0]=0x00; r[1]=0x4C; r[2]=1; r[3]=2; r[4]=0x00;r[5]=0x01;r[6]=0x02;r[7]=0x03;
    r[8]=0x3f;r[9]=0x80;r[10]=0x00;r[11]=0x00;
    beFixRecords(recs, 2, sizeof(Rec), bswapRec);
    chk("rec.a", recs[0].a, 76);
    chk("rec.b (byte untouched)", recs[0].b, 1);
    chk("rec.c (byte untouched)", recs[0].c, 2);
    chk("rec.d", (long)recs[0].d, 0x00010203);
    chk("rec.e==1.0", recs[0].e == 1.0f, 1);

    printf("\n%s (%d failure%s)\n", gFails ? "SMOKETEST FAILED" : "SMOKETEST PASSED",
           gFails, gFails == 1 ? "" : "s");
    return gFails ? 1 : 0;
}
