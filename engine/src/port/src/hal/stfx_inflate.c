// stfx_inflate.c - compact DEFLATE inflate (port of Mark Adler's public-domain
// "puff"). See stfx_inflate.h. Decodes the zlib streams inside SFA's ZLB blocks.

#include "port/stfx_inflate.h"
#include <setjmp.h>
#include <string.h>

#define MAXBITS   15
#define MAXLCODES 286
#define MAXDCODES 30
#define MAXCODES  (MAXLCODES + MAXDCODES)
#define FIXLCODES 288

struct state {
    unsigned char*       out;
    unsigned long        outlen;
    unsigned long        outcnt;
    const unsigned char* in;
    unsigned long        inlen;
    unsigned long        incnt;
    int                  bitbuf;
    int                  bitcnt;
    jmp_buf              env;
};

struct huffman { short* count; short* symbol; };

static int bits(struct state* s, int need) {
    long val = s->bitbuf;
    while (s->bitcnt < need) {
        if (s->incnt == s->inlen) longjmp(s->env, 1);
        val |= (long)(s->in[s->incnt++]) << s->bitcnt;
        s->bitcnt += 8;
    }
    s->bitbuf = (int)(val >> need);
    s->bitcnt -= need;
    return (int)(val & ((1L << need) - 1));
}

static int stored(struct state* s) {
    s->bitbuf = 0; s->bitcnt = 0;
    if (s->incnt + 4 > s->inlen) return 2;
    unsigned len = s->in[s->incnt++]; len |= s->in[s->incnt++] << 8;
    if (s->in[s->incnt++] != (~len & 0xff) || s->in[s->incnt++] != ((~len >> 8) & 0xff)) return -2;
    if (s->incnt + len > s->inlen) return 2;
    while (len--) {
        if (s->outcnt == s->outlen) return 1;
        s->out[s->outcnt++] = s->in[s->incnt++];
    }
    return 0;
}

static int decode(struct state* s, const struct huffman* h) {
    int code = 0, first = 0, index = 0;
    for (int len = 1; len <= MAXBITS; ++len) {
        code |= bits(s, 1);
        int count = h->count[len];
        if (code - count < first) return h->symbol[index + (code - first)];
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
    }
    return -10;
}

static int construct(struct huffman* h, const short* length, int n) {
    for (int len = 0; len <= MAXBITS; ++len) h->count[len] = 0;
    for (int sym = 0; sym < n; ++sym) h->count[length[sym]]++;
    if (h->count[0] == n) return 0;
    int left = 1;
    for (int len = 1; len <= MAXBITS; ++len) {
        left <<= 1;
        left -= h->count[len];
        if (left < 0) return left;
    }
    short offs[MAXBITS + 1];
    offs[1] = 0;
    for (int len = 1; len < MAXBITS; ++len) offs[len + 1] = offs[len] + h->count[len];
    for (int sym = 0; sym < n; ++sym)
        if (length[sym] != 0) h->symbol[offs[length[sym]]++] = (short)sym;
    return left;
}

static int codes(struct state* s, const struct huffman* lencode, const struct huffman* distcode) {
    static const short lens[29] = {3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258};
    static const short lext[29] = {0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0};
    static const short dists[30] = {1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577};
    static const short dext[30] = {0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13};
    int symbol;
    do {
        symbol = decode(s, lencode);
        if (symbol < 0) return symbol;
        if (symbol < 256) {
            if (s->outcnt == s->outlen) return 1;
            s->out[s->outcnt++] = (unsigned char)symbol;
        } else if (symbol > 256) {
            symbol -= 257;
            if (symbol >= 29) return -10;
            int len = lens[symbol] + bits(s, lext[symbol]);
            symbol = decode(s, distcode);
            if (symbol < 0) return symbol;
            unsigned dist = dists[symbol] + bits(s, dext[symbol]);
            if (dist > s->outcnt) return -11;
            if (s->outcnt + len > s->outlen) return 1;
            while (len--) { s->out[s->outcnt] = s->out[s->outcnt - dist]; s->outcnt++; }
        }
    } while (symbol != 256);
    return 0;
}

static int fixed(struct state* s) {
    static short lencnt[MAXBITS + 1], lensym[FIXLCODES];
    static short distcnt[MAXBITS + 1], distsym[MAXDCODES];
    static struct huffman lencode = {lencnt, lensym};
    static struct huffman distcode = {distcnt, distsym};
    static int built = 0;
    if (!built) {
        short lengths[FIXLCODES];
        int sym = 0;
        for (; sym < 144; ++sym) lengths[sym] = 8;
        for (; sym < 256; ++sym) lengths[sym] = 9;
        for (; sym < 280; ++sym) lengths[sym] = 7;
        for (; sym < FIXLCODES; ++sym) lengths[sym] = 8;
        construct(&lencode, lengths, FIXLCODES);
        for (sym = 0; sym < MAXDCODES; ++sym) lengths[sym] = 5;
        construct(&distcode, lengths, MAXDCODES);
        built = 1;
    }
    return codes(s, &lencode, &distcode);
}

static int dynamic(struct state* s) {
    static const short order[19] = {16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15};
    short lengths[MAXCODES];
    short lencnt[MAXBITS + 1], lensym[MAXLCODES];
    short distcnt[MAXBITS + 1], distsym[MAXDCODES];
    struct huffman lencode = {lencnt, lensym};
    struct huffman distcode = {distcnt, distsym};

    int nlen = bits(s, 5) + 257;
    int ndist = bits(s, 5) + 1;
    int ncode = bits(s, 4) + 4;
    if (nlen > MAXLCODES || ndist > MAXDCODES) return -3;

    int index = 0;
    for (; index < ncode; ++index) lengths[order[index]] = (short)bits(s, 3);
    for (; index < 19; ++index) lengths[order[index]] = 0;
    int err = construct(&lencode, lengths, 19);
    if (err != 0) return -4;

    index = 0;
    while (index < nlen + ndist) {
        int symbol = decode(s, &lencode);
        if (symbol < 0) return symbol;
        if (symbol < 16) { lengths[index++] = (short)symbol; }
        else {
            int len = 0;
            if (symbol == 16) { if (index == 0) return -5; len = lengths[index - 1]; symbol = 3 + bits(s, 2); }
            else if (symbol == 17) { symbol = 3 + bits(s, 3); }
            else { symbol = 11 + bits(s, 7); }
            if (index + symbol > nlen + ndist) return -6;
            while (symbol--) lengths[index++] = (short)len;
        }
    }
    if (lengths[256] == 0) return -9;
    err = construct(&lencode, lengths, nlen);
    if (err && (err < 0 || nlen != lencode.count[0] + lencode.count[1])) return -7;
    err = construct(&distcode, lengths + nlen, ndist);
    if (err && (err < 0 || ndist != distcode.count[0] + distcode.count[1])) return -8;
    return codes(s, &lencode, &distcode);
}

int stfx_inflate_raw(uint8_t* dst, size_t dstCap, const uint8_t* src, size_t srcLen, size_t* outLen) {
    struct state s;
    s.out = dst; s.outlen = (unsigned long)dstCap; s.outcnt = 0;
    s.in = src; s.inlen = (unsigned long)srcLen; s.incnt = 0;
    s.bitbuf = 0; s.bitcnt = 0;
    int err;
    if (setjmp(s.env) != 0) {
        err = 2; // ran out of input
    } else {
        int last, type;
        err = 0;
        do {
            last = bits(&s, 1);
            type = bits(&s, 2);
            err = type == 0 ? stored(&s) : type == 1 ? fixed(&s) : type == 2 ? dynamic(&s) : -1;
            if (err != 0) break;
        } while (!last);
    }
    if (outLen) *outLen = s.outcnt;
    return err;
}

int stfx_inflate_zlib(uint8_t* dst, size_t dstCap, const uint8_t* src, size_t srcLen, size_t* outLen) {
    if (srcLen < 2) return -1;
    // zlib header: CMF, FLG. CM (low nibble of CMF) must be 8 (deflate).
    if ((src[0] & 0x0f) != 8) return -1;
    return stfx_inflate_raw(dst, dstCap, src + 2, srcLen - 2, outLen);
}
