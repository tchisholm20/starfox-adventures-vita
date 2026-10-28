// assets.c - Star Fox Adventures asset readers (see assets.h). On-disc format layer.

#include "port/assets.h"
#include "port/stfx_inflate.h"

#include <string.h>
#include <stdlib.h>

const int ASSET_POS_FRAC[8] = {0,2,0,8,0,3,8,0};

// --- ZLB -------------------------------------------------------------------
int asset_findZLB(const uint8_t* p, int maxScan) {
    for (int i=0; i+4<=maxScan; ++i)
        if (p[i]=='Z'&&p[i+1]=='L'&&p[i+2]=='B'&&p[i+3]==0) return i;
    return -1;
}
size_t asset_inflateZLB(const uint8_t* zlb, int avail, uint8_t* out, size_t cap) {
    if (avail < 0x10 || memcmp(zlb,"ZLB",3)!=0) return 0;
    unsigned usize=asset_be32(zlb+8), csize=asset_be32(zlb+0xc);
    if (usize==0 || usize>cap || 0x10+(int)csize>avail) return 0;
    size_t got=0;
    if (stfx_inflate_zlib(out, cap, zlb+0x10, csize, &got)!=0 || got!=usize) return 0;
    return got;
}

// --- textures --------------------------------------------------------------
int asset_loadTexRecord(const uint8_t* tab,int tabSize,const uint8_t* bin,int binSize,int id,
                        uint8_t* out,size_t cap,int* ow,int* oh,int* ofmt,unsigned* oimgOff) {
    if (!tab||!bin||id<0||(id*4+4)>tabSize) return 0;
    unsigned e=asset_be32(tab+id*4); int mips=(e>>24)&0x3f; unsigned off=(e&0xffffff)<<1;
    if (mips<1 || off+0x20>(unsigned)binSize) return 0;
    const uint8_t* r=bin+off;
    if (memcmp(r,"ZLB",3)==0) {                          // deflate-compressed
        if (asset_inflateZLB(r, binSize-(int)off, out, cap)==0) return 0;
    } else if (memcmp(r,"DIR",3)==0) {                   // stored raw, texture at +0x20
        unsigned usize=asset_be32(r+8);
        if (usize<=0x20) return 0;
        unsigned texSize=usize-0x20;
        if (off+0x20+texSize>(unsigned)binSize || texSize>cap) return 0;
        memcpy(out, r+0x20, texSize);
    } else return 0;
    int w=(int)asset_be16(out+0xA), h=(int)asset_be16(out+0xC), fmt=out[0x16];
    unsigned imgOff=0x60+(unsigned)(int)asset_be32(out+0x50);
    if (w<4||w>2048||h<4||h>2048||imgOff>=cap) return 0;
    *ow=w; *oh=h; *ofmt=fmt; *oimgOff=imgOff; return 1;
}

// --- map blocks ------------------------------------------------------------
// MapBlockData field offsets (include/main/map_block.h).
enum { MB_TRANSFORM=0x0C, MB_GCPOLYS=0x4C, MB_TEXARR=0x54, MB_VERTS=0x58, MB_COLORS=0x5C,
       MB_TEXCOORDS=0x60, MB_SHADERS=0x64, MB_DLISTS=0x68, MB_YOFF=0x8E, MB_VERTCOUNT=0x90,
       MB_COLORCOUNT=0x94, MB_TEXCOORDCOUNT=0x96, MB_NPOLYS=0x98, MB_TEXCNT=0xA0,
       MB_DLCOUNT=0xA1, MB_SHCOUNT=0xA2 };

unsigned asset_mapBlockOffset(const uint8_t* tab, int tabSize, int blockId) {
    if (blockId<0 || blockId*4+4>tabSize) return ASSET_NO_BLOCK;
    unsigned raw=asset_be32(tab+blockId*4);
    return raw ? (raw & 0x00ffffff) : ASSET_NO_BLOCK;   // low 24 bits; offset 0 is valid
}

// A display list's per-vertex layout is POS/CLR0/TEX0 indices, each u8 or u16. Detect
// the three sizes by parsing DL0 with each candidate stride and checking it lands
// exactly on dlistSize with in-range indices.
static int detectIndexSizes(const uint8_t* dl,int dlSize,int vc,int cc,int tc,int* pS,int* cS,int* tS) {
    for (int ps=2; ps>=1; --ps) for (int cs=1; cs<=2; ++cs) for (int ts=2; ts>=1; --ts) {
        int stride=ps+cs+ts, pos=0, ok=1, any=0, checked=0, good=0;
        while (pos < dlSize) {
            unsigned op=dl[pos++]; if(op==0) continue;
            unsigned prim=op&0xF8;
            if (!(prim==0x80||prim==0x90||prim==0x98||prim==0xA0)) { ok=0; break; }
            if (pos+2>dlSize){ ok=0; break; }
            int n=(dl[pos]<<8)|dl[pos+1]; pos+=2; any=1;
            for (int v=0; v<n && checked<64; ++v){ const uint8_t* vp=dl+pos+v*stride;
                int pi=ps==2?((vp[0]<<8)|vp[1]):vp[0];
                int ci=cs==2?((vp[ps]<<8)|vp[ps+1]):vp[ps];
                int ti=ts==2?((vp[ps+cs]<<8)|vp[ps+cs+1]):vp[ps+cs];
                checked++; if(pi<vc && (cc==0||ci<cc) && (tc==0||ti<tc)) good++; }
            pos += n*stride;
        }
        if (ok && any && pos==dlSize && checked>0 && good==checked){ *pS=ps; *cS=cs; *tS=ts; return 1; }
    }
    return 0;
}

int asset_loadMapBlock(const uint8_t* bin,int binSize,const uint8_t* tab,int tabSize,
                       int blockId,AssetMapBlock* blk) {
    unsigned off=asset_mapBlockOffset(tab,tabSize,blockId);
    if (off==ASSET_NO_BLOCK || off+0x10>(unsigned)binSize) return 0;
    const uint8_t* z=bin+off; if(memcmp(z,"ZLB",3)!=0) return 0;
    unsigned usize=asset_be32(z+8);
    if (usize<0xA4 || usize>0x32000) return 0;
    uint8_t* d=(uint8_t*)malloc(usize); if(!d) return 0;
    if (asset_inflateZLB(z, binSize-(int)off, d, usize)!=usize){ free(d); return 0; }

    memset(blk,0,sizeof *blk);
    blk->data=d; blk->size=usize; blk->id=blockId;
    blk->vertCount    =(int)asset_be16(d+MB_VERTCOUNT);
    blk->colorCount   =(int)asset_be16(d+MB_COLORCOUNT);
    blk->texCoordCount=(int)asset_be16(d+MB_TEXCOORDCOUNT);
    blk->nPolys       =(int)asset_be16(d+MB_NPOLYS);
    blk->vertOff=asset_be32(d+MB_VERTS); blk->colOff=asset_be32(d+MB_COLORS); blk->texOff=asset_be32(d+MB_TEXCOORDS);
    blk->polyOff=asset_be32(d+MB_GCPOLYS);
    blk->dlBase=asset_be32(d+MB_DLISTS); blk->dlCount=d[MB_DLCOUNT];
    blk->texArrOff=asset_be32(d+MB_TEXARR); blk->texCount=d[MB_TEXCNT];
    blk->shOff=asset_be32(d+MB_SHADERS);    blk->shCount=d[MB_SHCOUNT];
    blk->yOff=asset_s16be(d+MB_YOFF);

    if (blk->vertCount<3 || blk->vertCount>60000 || blk->dlCount<1 ||
        blk->vertOff==0 || blk->vertOff+(unsigned)blk->vertCount*6>usize ||
        blk->dlBase==0 || blk->dlBase+(unsigned)blk->dlCount*0x1C>usize) { free(d); blk->data=0; return 0; }
    if (blk->colOff+(unsigned)blk->vertCount*2>usize) blk->colOff=0;
    if (blk->texOff+(unsigned)blk->vertCount*4>usize) blk->texOff=0;

    unsigned dl0=asset_be32(d+blk->dlBase); int dl0sz=asset_be16(d+blk->dlBase+4);
    blk->fmt=(dl0sz>0)?(d[dl0]&7):5;
    blk->posSz=2; blk->clrSz=1; blk->texSz=2;
    detectIndexSizes(d+dl0, dl0sz, blk->vertCount, blk->colorCount, blk->texCoordCount,
                     &blk->posSz, &blk->clrSz, &blk->texSz);
    return 1;
}

void asset_freeMapBlock(AssetMapBlock* blk) { if(blk && blk->data){ free(blk->data); blk->data=0; } }
