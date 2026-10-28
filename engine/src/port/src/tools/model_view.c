// model_view.c - render a REAL Star Fox Adventures model from the ISO, using a
// faithful decode of SFA's render-instruction stream.
//
// Pipeline: mount ISO -> read MODELS.tab/MODELS.bin -> per record: FACEFEED wrapper
// + ZLB(zlib) -> ModelFileHeader + data. Model-relative offsets: vertices@0x28
// (stride6 s16 xyz), texCoords@0x34 (stride4 s16, VTXFMT0 frac7 -> raw/128),
// displayLists@0xD0 (0x1C entries {dlistOff@0,dlistSize@4}), renderOps@0x38
// (Shader[], stride 0x44), textureIds@0x20, instrs@0xD4, instrsBitLenWords@0xD8;
// counts vertexCount@0xE4 texCoordCount@0xEA jointCount@0xF3 textureCount@0xF2.
//
// The render-instruction stream (model->instrs, LSB-first bits) is 4-bit opcodes:
//   op1: +6b renderOp index -> current Shader (vtxAttrFlags@0x40, layerCount@0x41,
//        layers[0].textureIndex@0x24 -> textureIds[idx-1] -> TEX1 id)
//   op3: vtx descriptor -> +1b POS idx16, +1b NRM (if vtxAttrFlags&1),
//        +1b CLR0 (if vtxAttrFlags&2), +1b TEX (all layers) -> exact vertex layout
//   op4: +4b count, then count x +8b matrix index (skinning; bits consumed)
//   op2: +8b display-list index -> parse that DL with the current layout + texture
//   op5: end
// This yields, per display list, the exact vertex stride / POS+TEX offsets and the
// bound texture - so models render with their own multiple textures. If the decoded
// layout fails to validate against a DL (e.g. skinned jointCount>1 with runtime
// texmtx attrs), that DL falls back to an auto-detected POS/TEX layout.
//
//   model_view --iso <path> [--dir animtest] [--index N] [--gfx d3d11|d3d12|vk] [--capture out.bmp]

#include "port/gx_shim.h"
#include "port/dvd_shim.h"
#include "port/stfx_inflate.h"
#include "port/renderer/rhi.h"
#include "port/plat_window.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>

#if defined(_WIN32)
#include <windows.h>
static void sleep_ms(int ms) { Sleep((DWORD)ms); }
#endif

static unsigned be16(const unsigned char* p){ return (p[0]<<8)|p[1]; }
static int      s16be(const unsigned char* p){ int v=(p[0]<<8)|p[1]; return v>=0x8000?v-0x10000:v; }
static unsigned be32(const unsigned char* p){ return (p[0]<<24)|(p[1]<<16)|(p[2]<<8)|p[3]; }
static float f32be(const unsigned char* p){ union{unsigned u; float f;} x; x.u=be32(p); return x.f; }

static RhiBackend parse_backend(const char* s){
    if(!s) return RHI_BACKEND_D3D11;
    if(!strcmp(s,"vk")||!strcmp(s,"vulkan")) return RHI_BACKEND_VULKAN;
    if(!strcmp(s,"d3d12")) return RHI_BACKEND_D3D12;
    return RHI_BACKEND_D3D11;
}

// LSB-first bit reader matching the game (3 bytes LE from byte offset, shift, mask).
typedef struct { const unsigned char* d; int pos; int bitLen; } Bits;
static int readBits(Bits* b, int n){
    int pos=b->pos, off=pos>>3;
    unsigned w = b->d[off] | (b->d[off+1]<<8) | (b->d[off+2]<<16);
    b->pos = pos+n;
    return (int)((w >> (pos&7)) & ((1u<<n)-1));
}

static int findZLB(const unsigned char* p, int maxScan){
    for (int i=0;i+4<=maxScan;++i) if(p[i]=='Z'&&p[i+1]=='L'&&p[i+2]=='B'&&p[i+3]==0) return i;
    return -1;
}
static size_t decompressModel(const unsigned char* bin,int binSize,unsigned off,unsigned char* out,size_t cap){
    if(off+0x40>(unsigned)binSize) return 0;
    int z=findZLB(bin+off,0x40); if(z<0) return 0;
    const unsigned char* zlb=bin+off+z;
    unsigned usize=be32(zlb+8),csize=be32(zlb+0xc);
    if(usize==0||usize>cap||off+z+0x10+csize>(unsigned)binSize) return 0;
    size_t got=0; int rc=stfx_inflate_zlib(out,cap,zlb+0x10,csize,&got);
    if(rc!=0 && got<cap) return 0;
    if(cap>=usize && got!=usize) return 0;
    return got;
}
// Decompress a TEX1 record by id -> GC image + dims/format into a caller buffer.
static int loadTex1(unsigned char* tab,int tabSize,unsigned char* bin,int binSize,int id,
                    unsigned char* out,size_t outCap,int* ow,int* oh,int* ofmt,unsigned* oimgOff){
    if(!tab||!bin||id<0||(id*4+4)>tabSize) return 0;
    unsigned e=be32(tab+id*4); int mips=(e>>24)&0x3f; unsigned off=(e&0xffffff)<<1;
    if(mips<1||off==0||off+0x10>(unsigned)binSize) return 0;
    const unsigned char* r=bin+off;
    if(mips>1 && memcmp(r,"ZLB",3)!=0){ off+=be32(r); if(off+0x10>(unsigned)binSize) return 0; r=bin+off; }
    if(memcmp(r,"ZLB",3)!=0) return 0;
    unsigned usize=be32(r+8),csize=be32(r+0xc);
    if(usize<0x60||usize>outCap||off+0x10+csize>(unsigned)binSize) return 0;
    size_t got=0;
    if(stfx_inflate_zlib(out,outCap,r+0x10,csize,&got)!=0||got!=usize) return 0;
    int w=(int)be16(out+0xA),h=(int)be16(out+0xC),fmt=out[0x16];
    unsigned imgOff=0x60+(unsigned)(int)be32(out+0x50);
    if(w<4||w>1024||h<4||h>1024||imgOff>=usize) return 0;
    *ow=w; *oh=h; *ofmt=fmt; *oimgOff=imgOff;
    return 1;
}

typedef struct { unsigned off; int vc,dc,jc; unsigned vtxOff,dlOff,usize; } Model;

// --- per-texture draw groups -----------------------------------------------
typedef struct {
    int texId;
    RhiTexVertex* v; int n, cap;
    GXTexObj obj; unsigned char* texBuf; int haveTex, w, h, fmt; unsigned imgOff;
} Group;
#define MAX_GROUPS 128
static Group gGroups[MAX_GROUPS]; static int gNGroups;
static RhiColorVertex* gFlat; static int gNFlat, gFlatCap;

static Group* groupFor(int texId){
    for(int i=0;i<gNGroups;++i) if(gGroups[i].texId==texId) return &gGroups[i];
    if(gNGroups>=MAX_GROUPS) return &gGroups[0];
    Group* g=&gGroups[gNGroups++]; memset(g,0,sizeof(*g)); g->texId=texId; return g;
}
static void gpush(Group* g, RhiTexVertex vv){
    if(g->n==g->cap){ g->cap=g->cap?g->cap*2:8192; g->v=(RhiTexVertex*)realloc(g->v,(size_t)g->cap*sizeof(RhiTexVertex)); }
    g->v[g->n++]=vv;
}
static void fpush(RhiColorVertex vv){
    if(gNFlat==gFlatCap){ gFlatCap=gFlatCap?gFlatCap*2:8192; gFlat=(RhiColorVertex*)realloc(gFlat,(size_t)gFlatCap*sizeof(RhiColorVertex)); }
    gFlat[gNFlat++]=vv;
}

// emit context. Positions are stored raw (world = local + joint offset) and
// normalized in a post-pass, since bind-pose offsets define the real bounds.
static const unsigned char* gVtx; static const unsigned char* gTcs;
#define MAX_JOINTS 512
static float gOff[MAX_JOINTS][3]; static int gHaveJoints; static int gSlot[16];
static void poseVert(int idx,int joint,float o[3]){
    const unsigned char* q=gVtx+idx*6;
    o[0]=(float)s16be(q)/256.0f; o[1]=(float)s16be(q+2)/256.0f; o[2]=(float)s16be(q+4)/256.0f;
    if(gHaveJoints && joint>=0 && joint<MAX_JOINTS){ o[0]+=gOff[joint][0]; o[1]+=gOff[joint][1]; o[2]+=gOff[joint][2]; }
}
static void emitTexTri(Group* g,int a,int b,int c,int ta,int tb,int tc,int ja,int jb,int jc_){
    int pi[3]={a,b,c}, ti[3]={ta,tb,tc}, ji[3]={ja,jb,jc_}; float p[3][3];
    for(int k=0;k<3;++k) poseVert(pi[k],ji[k],p[k]);
    for(int k=0;k<3;++k){ float u=(float)s16be(gTcs+ti[k]*4)/1024.0f, v=(float)s16be(gTcs+ti[k]*4+2)/1024.0f;
        RhiTexVertex vv={p[k][0],p[k][1],p[k][2],0xFFFFFFFFu,u,v}; gpush(g,vv); }
}
static void emitFlatTri(int a,int b,int c,int ja,int jb,int jc_){
    int pi[3]={a,b,c}, ji[3]={ja,jb,jc_}; float p[3][3];
    for(int k=0;k<3;++k) poseVert(pi[k],ji[k],p[k]);
    float ux=p[1][0]-p[0][0],uy=p[1][1]-p[0][1],uz=p[1][2]-p[0][2];
    float vx=p[2][0]-p[0][0],vy=p[2][1]-p[0][1],vz=p[2][2]-p[0][2];
    float nx=uy*vz-uz*vy,ny=uz*vx-ux*vz,nz=ux*vy-uy*vx; float nl=sqrtf(nx*nx+ny*ny+nz*nz); if(nl<1e-6f)nl=1;
    float diff=0.30f+0.70f*fabsf((nx*0.4f+ny*0.5f+nz*0.75f)/nl);
    unsigned gg=(unsigned)(diff*215)+20; unsigned rgba=gg|(gg<<8)|(gg<<16)|(255u<<24);
    for(int k=0;k<3;++k){ RhiColorVertex vv={p[k][0],p[k][1],p[k][2],rgba}; fpush(vv); }
}

// Validate a candidate (posOff,posSz,texOff,texSz,stride) against a display list.
static int validateLayout(const unsigned char* dl,int dlSize,int posOff,int posSz,
                          int texOff,int texSz,int stride,int vc,int tcc,int wantTex){
    int p=0, saw=0;
    while(p<dlSize){ unsigned op=dl[p]; if(op==0){++p;continue;} if((op&0x80)==0) return 0;
        if(p+3>dlSize) return 0; int cnt=be16(dl+p+1); p+=3;
        if(cnt==0||p+cnt*stride>dlSize) return 0;
        for(int v=0;v<cnt;++v){
            int pi=(posSz==2)?be16(dl+p+v*stride+posOff):dl[p+v*stride+posOff]; if(pi>=vc) return 0;
            if(wantTex){ int ti=(texSz==2)?be16(dl+p+v*stride+texOff):dl[p+v*stride+texOff]; if(ti>=tcc) return 0; }
        }
        p+=cnt*stride; saw=1;
    }
    return saw;
}

// Parse a display list into groups, using an explicit layout (validated by caller).
// hasJoints: read the leading PNMTXIDX byte per vertex and map slot->joint (gSlot).
static void parseDL(const unsigned char* dl,int dlSize,int posOff,int posSz,
                    int texOff,int texSz,int stride,int hasTex,int hasJoints,Group* g){
    int p=0;
    while(p<dlSize){ unsigned op=dl[p]; if(op==0){++p;continue;} if((op&0x80)==0) break;
        int prim=op&0xf8; int cnt=be16(dl+p+1); p+=3;
        int n=cnt>4096?4096:cnt; static int pidx[4096], tidx[4096], jidx[4096];
        for(int v=0;v<n;++v){
            const unsigned char* vp=dl+p+v*stride;
            pidx[v]=(posSz==2)?be16(vp+posOff):vp[posOff];
            tidx[v]=hasTex?((texSz==2)?be16(vp+texOff):vp[texOff]):0;
            jidx[v]=hasJoints?gSlot[(vp[0]/3)&15]:-1; // PNMTXIDX at vertex offset 0
        }
        p+=cnt*stride;
        #define T(a,b,c) do{ if(hasTex&&g) emitTexTri(g,pidx[a],pidx[b],pidx[c],tidx[a],tidx[b],tidx[c],jidx[a],jidx[b],jidx[c]); \
                             else emitFlatTri(pidx[a],pidx[b],pidx[c],jidx[a],jidx[b],jidx[c]); }while(0)
        if(prim==0x90){ for(int v=0;v+3<=n;v+=3) T(v,v+1,v+2); }
        else if(prim==0x98){ for(int v=2;v<n;++v){ if(v&1) T(v-1,v-2,v); else T(v-2,v-1,v); } }
        else if(prim==0xa0){ for(int v=2;v<n;++v) T(0,v-1,v); }
        else if(prim==0x80){ for(int v=0;v+4<=n;v+=4){ T(v,v+1,v+2); T(v,v+2,v+3); } }
        #undef T
    }
}

// Auto-detect POS(+optional TEX) layout for a DL (fallback for skinned models).
static int autoDetect(const unsigned char* dl,int dlSize,int vc,int tcc,int wantTex,
                      int* posOff,int* posSz,int* texOff,int* texSz,int* stride){
    for(int ps=2;ps>=1;--ps) for(int st=ps;st<=48;++st){
        if(!validateLayout(dl,dlSize,0,ps,0,0,st,vc,tcc,0)) continue;
        *posOff=0; *posSz=ps; *stride=st;
        if(wantTex){ for(int ts=2;ts>=1;--ts) for(int to=st-ts;to>=ps;--to)
            if(validateLayout(dl,dlSize,0,ps,to,ts,st,vc,tcc,1)){ *texOff=to; *texSz=ts; return 2; } }
        *texOff=-1; *texSz=0; return 1;
    }
    return 0;
}

int main(int argc, char** argv) {
    const char* iso=NULL; const char* dir="animtest"; const char* gfx="d3d11";
    const char* capturePath=NULL; int wantIndex=-1;
    for (int i=1;i<argc;++i){
        if(!strcmp(argv[i],"--iso")&&i+1<argc) iso=argv[++i];
        else if(!strcmp(argv[i],"--dir")&&i+1<argc) dir=argv[++i];
        else if(!strcmp(argv[i],"--index")&&i+1<argc) wantIndex=atoi(argv[++i]);
        else if(!strcmp(argv[i],"--gfx")&&i+1<argc) gfx=argv[++i];
        else if(!strcmp(argv[i],"--capture")&&i+1<argc) capturePath=argv[++i];
    }
    if(!iso){ fprintf(stderr,"usage: model_view --iso <path> [--dir animtest] [--index N]\n"); return 2; }

    dvd_shim_init(iso);
    char tabPath[256],binPath[256],t1tabP[256],t1binP[256];
    snprintf(tabPath,sizeof(tabPath),"%s/MODELS.tab",dir);
    snprintf(binPath,sizeof(binPath),"%s/MODELS.bin",dir);
    snprintf(t1tabP,sizeof(t1tabP),"%s/TEX1.tab",dir);
    snprintf(t1binP,sizeof(t1binP),"%s/TEX1.bin",dir);
    int tabSize=0,binSize=0,t1tabSize=0,t1binSize=0;
    unsigned char* tab=(unsigned char*)loadFileByPath(tabPath,&tabSize,0);
    unsigned char* bin=(unsigned char*)loadFileByPath(binPath,&binSize,0);
    unsigned char* t1tab=(unsigned char*)loadFileByPath(t1tabP,&t1tabSize,0);
    unsigned char* t1bin=(unsigned char*)loadFileByPath(t1binP,&t1binSize,0);
    if(!tab||!bin){ fprintf(stderr,"[model] cannot read %s/%s\n",tabPath,binPath); return 3; }
    printf("[model] %s: tab=%d (%d) bin=%d\n",dir,tabSize,tabSize/4,binSize);

    // Collect valid models (header probe of each distinct ZLB).
    static unsigned char probe[1024];
    int nT=tabSize/4; Model* models=(Model*)malloc(sizeof(Model)*nT); int nModels=0; unsigned last=0xffffffff;
    for(int i=0;i<nT;++i){ unsigned e=be32(tab+i*4); unsigned off=e&0xffffff; // MODELS.tab: direct byte offset
        if(off==last||off==0||off>=(unsigned)binSize) continue; last=off;
        int z=(off+0x40<=(unsigned)binSize)?findZLB(bin+off,0x40):-1; if(z<0) continue;
        const unsigned char* zlb=bin+off+z; unsigned usize=be32(zlb+8),csize=be32(zlb+0xc);
        if(usize<0x100||off+z+0x10+csize>(unsigned)binSize) continue;
        size_t got=0; stfx_inflate_zlib(probe,sizeof(probe),zlb+0x10,csize,&got); if(got<0x100) continue;
        int vc=be16(probe+0xE4),dc=probe[0xF5],jc=probe[0xF3];
        unsigned vtxOff=be32(probe+0x28),dlOff=be32(probe+0xD0);
        if(vc<3||vc>60000||dc<1||dc>250) continue;
        if(vtxOff==0||vtxOff+(unsigned)vc*6>usize||dlOff==0||dlOff+(unsigned)dc*0x1c>usize) continue;
        models[nModels].off=off;models[nModels].vc=vc;models[nModels].dc=dc;models[nModels].jc=jc;
        models[nModels].vtxOff=vtxOff;models[nModels].dlOff=dlOff;models[nModels].usize=usize;++nModels;
    }
    printf("[model] %d models. First few:\n",nModels);
    for(int i=0;i<nModels&&i<16;++i) printf("   [%2d] verts=%d dls=%d joints=%d\n",i,models[i].vc,models[i].dc,models[i].jc);
    if(nModels==0){ fprintf(stderr,"[model] none found\n"); return 4; }

    int pick=-1;
    if(wantIndex>=0&&wantIndex<nModels) pick=wantIndex;
    else { long best=-1; for(int i=0;i<nModels;++i){ if(models[i].jc>1)continue; if(models[i].vc>best){best=models[i].vc;pick=i;} } }
    if(pick<0) pick=0;
    Model M=models[pick];
    printf("[model] showing [%d] verts=%d dls=%d joints=%d\n",pick,M.vc,M.dc,M.jc);

    unsigned char* m=(unsigned char*)malloc(M.usize);
    if(!m||decompressModel(bin,binSize,M.off,m,M.usize)!=M.usize){ fprintf(stderr,"[model] decompress failed\n"); return 5; }

    const unsigned char* vtx=m+M.vtxOff;
    unsigned tcOff=be32(m+0x34); gTcs=m+tcOff; int tcCount=(int)be16(m+0xEA);
    int tcValid=(tcOff>0&&tcOff+(unsigned)tcCount*4<=M.usize&&tcCount>0);
    unsigned renderOpsOff=be32(m+0x38); int renderOpCount=m[0xF8];
    unsigned tidOff=be32(m+0x20); int textureCount=m[0xF2];
    unsigned instrsOff=be32(m+0xD4); int instrBitLen=(int)be16(m+0xD8)<<3;
    int jc=M.jc;

    gVtx=vtx;
    for(int i=0;i<16;++i) gSlot[i]=i; // default identity slot->joint

    // Bind-pose joint offsets: worldPos = localPos + (accumHead - tail), where
    // accumHead sums the bone head translations down the parent chain. Extra joints
    // (jointCount..+extraJointCount) are translation-blends of two joints.
    gHaveJoints=0;
    unsigned jdOff=be32(m+0x3C); int jointCount=m[0xF3], extraCount=m[0xF4];
    static float accum[MAX_JOINTS][3];
    if(jointCount>0 && jointCount<=MAX_JOINTS && jdOff>0 && jdOff+(unsigned)jointCount*0x1c<=M.usize){
        for(int j=0;j<jointCount;++j){
            const unsigned char* b=m+jdOff+j*0x1c; int parent=(signed char)b[0];
            float head[3],tail[3];
            for(int c=0;c<3;++c){ head[c]=f32be(b+4+c*4); tail[c]=f32be(b+0x10+c*4); }
            for(int c=0;c<3;++c){ accum[j][c]=head[c] + ((parent>=0&&parent<j)?accum[parent][c]:0.0f); gOff[j][c]=tail[c]; }
        }
        unsigned exOff=be32(m+0x54);
        for(int i=0;i<extraCount && (jointCount+i)<MAX_JOINTS; ++i){
            if(exOff+(unsigned)i*3+3>M.usize) break;
            const unsigned char* e=m+exOff+i*3; int g0=e[0],g1=e[1]; float w=e[2]/4.0f, wi=1.0f-w;
            for(int c=0;c<3;++c) gOff[jointCount+i][c]=0.0f; (void)g0;(void)g1;(void)w;(void)wi;
        }
        gHaveJoints=(jc>1);
    }

    // Walk the render-instruction stream.
    Bits bs={ m+instrsOff, 0, instrBitLen };
    const unsigned char* curOp=NULL; int curTexId=-1;
    int posSz=2,nrmP=0,nrmSz=1,clrP=0,clrSz=1,texSz=1,layerCount=0;
    int guard=0;
    if (instrsOff>0 && instrsOff<M.usize && instrBitLen>0){
        while(bs.pos+4<=bs.bitLen && guard++<100000){
            int op=readBits(&bs,4);
            if(op==1){
                int idx=readBits(&bs,6);
                if((unsigned)renderOpsOff+ (unsigned)(idx+1)*0x44 <= M.usize && idx<renderOpCount){
                    curOp=m+renderOpsOff+idx*0x44;
                    int ti=(int)be32(curOp+0x24);
                    curTexId=-1;
                    // On disc, textureIndex is a 0-based index into the model's textureIds[].
                    if(ti>=0 && ti<textureCount && tidOff+(unsigned)ti*4+4<=M.usize)
                        curTexId=(int)be32(m+tidOff+ti*4);
                } else curOp=NULL;
            } else if(op==3){
                posSz=readBits(&bs,1)?2:1;
                nrmP=0; if(curOp&&(curOp[0x40]&1)){ nrmP=1; nrmSz=readBits(&bs,1)?2:1; }
                clrP=0; if(curOp&&(curOp[0x40]&2)){ clrP=1; clrSz=readBits(&bs,1)?2:1; }
                texSz=readBits(&bs,1)?2:1; layerCount=curOp?curOp[0x41]:0;
            } else if(op==4){
                int cnt=readBits(&bs,4); for(int i=0;i<cnt;++i){ int mi=readBits(&bs,8); if(i<16) gSlot[i]=mi; }
            } else if(op==2){
                int dlIdx=readBits(&bs,8);
                if(dlIdx<M.dc){
                    const unsigned char* de=m+M.dlOff+dlIdx*0x1c;
                    unsigned dlo=be32(de+0); int dls=(int)be16(de+4);
                    if(dlo>0 && dlo+dls<=M.usize){
                        const unsigned char* dl=m+dlo;
                        // exact layout from the descriptor. Skinned models prepend a
                        // PNMTXIDX byte + one texmtx-index byte per texMtxCount.
                        int pnmtx=(jc>1)?(1 + ((curOp&&curOp[0x41]>0) ? 1 + (be32(curOp+0x34)!=0xFFFFFFFFu) : 0) + m[0xFA]):0;
                        int posOff=pnmtx, nrmOff=posOff+posSz, clrOff=nrmOff+(nrmP?nrmSz:0), texOff=clrOff+(clrP?clrSz:0);
                        int stride=texOff + (layerCount>0?layerCount*texSz:0);
                        int hasTex=(layerCount>0 && curTexId>=0 && tcValid);
                        Group* g = hasTex ? groupFor(curTexId) : NULL;
                        if(validateLayout(dl,dls,posOff,posSz,texOff,texSz,stride,M.vc,tcCount,hasTex)){
                            parseDL(dl,dls,posOff,posSz,texOff,texSz,stride,hasTex,jc>1,g);
                        } else {
                            int aPosOff,aPosSz,aTexOff,aTexSz,aStride;
                            int r=autoDetect(dl,dls,M.vc,tcCount,hasTex,&aPosOff,&aPosSz,&aTexOff,&aTexSz,&aStride);
                            if(r>=1){ int at=(r==2); parseDL(dl,dls,aPosOff,aPosSz,aTexOff,aTexSz,aStride, at&&hasTex, 0, at&&hasTex?g:NULL); }
                        }
                    }
                }
            } else if(op==5){ break; }
            else break;
        }
    }
    // If the instr stream produced nothing (odd model), fall back to raw DL scan.
    long totalTex=0; for(int i=0;i<gNGroups;++i) totalTex+=gGroups[i].n;
    if(totalTex==0 && gNFlat==0){
        for(int d=0; d<M.dc; ++d){ const unsigned char* de=m+M.dlOff+d*0x1c;
            unsigned dlo=be32(de+0); int dls=(int)be16(de+4); if(dlo==0||dlo+dls>M.usize) continue;
            int a,b,c,dd,ee; if(autoDetect(m+dlo,dls,M.vc,tcCount,0,&a,&b,&c,&dd,&ee)) parseDL(m+dlo,dls,a,b,c,dd,ee,0,0,NULL); }
    }
    // Normalize all emitted (posed) world positions into the view.
    { float mn[3]={1e9f,1e9f,1e9f}, mx[3]={-1e9f,-1e9f,-1e9f};
      #define ACC(X,Y,Z) do{ if((X)<mn[0])mn[0]=(X); if((X)>mx[0])mx[0]=(X); if((Y)<mn[1])mn[1]=(Y); if((Y)>mx[1])mx[1]=(Y); if((Z)<mn[2])mn[2]=(Z); if((Z)>mx[2])mx[2]=(Z);}while(0)
      for(int i=0;i<gNGroups;++i) for(int k=0;k<gGroups[i].n;++k){ RhiTexVertex* v=&gGroups[i].v[k]; ACC(v->x,v->y,v->z); }
      for(int k=0;k<gNFlat;++k){ RhiColorVertex* v=&gFlat[k]; ACC(v->x,v->y,v->z); }
      #undef ACC
      float ctr[3],ext=1e-6f; for(int c=0;c<3;++c){ ctr[c]=(mn[c]+mx[c])*0.5f; float e=mx[c]-mn[c]; if(e>ext)ext=e; }
      float sc=1.6f/ext;
      for(int i=0;i<gNGroups;++i) for(int k=0;k<gGroups[i].n;++k){ RhiTexVertex* v=&gGroups[i].v[k]; v->x=(v->x-ctr[0])*sc; v->y=(v->y-ctr[1])*sc; v->z=(v->z-ctr[2])*sc; }
      for(int k=0;k<gNFlat;++k){ RhiColorVertex* v=&gFlat[k]; v->x=(v->x-ctr[0])*sc; v->y=(v->y-ctr[1])*sc; v->z=(v->z-ctr[2])*sc; }
    }
    printf("[model] %d texture groups, %d flat tris\n", gNGroups, gNFlat/3);

    // Decode each group's texture from TEX1.
    for(int i=0;i<gNGroups;++i){ Group* g=&gGroups[i];
        g->texBuf=(unsigned char*)malloc(2*1024*1024);
        if(g->texBuf && loadTex1(t1tab,t1tabSize,t1bin,t1binSize,g->texId,g->texBuf,2*1024*1024,&g->w,&g->h,&g->fmt,&g->imgOff)){
            g->haveTex=1; GXInitTexObj(&g->obj, g->texBuf+g->imgOff,(uint16_t)g->w,(uint16_t)g->h,g->fmt,0,0,0);
        }
        printf("   group tex=%d %dx%d fmt=%d tris=%d %s\n", g->texId,g->w,g->h,g->fmt,g->n/3, g->haveTex?"":"(decode failed)");
    }

    const int W=1280,H=720;
    char title[160]; snprintf(title,sizeof(title),"Stairfax Temperatures - %s MODEL [%d] [%s]",dir,pick,gfx);
    PlatWindow* win=plat_window_create(title,W,H);
    RhiCreateInfo ci={0}; ci.backend=parse_backend(gfx); ci.windowHandle=plat_window_native_handle(win);
    ci.width=W; ci.height=H; ci.vsync=true; ci.appName="Stairfax Temperatures";
    RhiInstance* rhi=rhi_create(&ci); if(!rhi){ fprintf(stderr,"[model] rhi_create failed\n"); return 7; }
    RhiSwapchain* sc=rhi_swapchainCreate(rhi,ci.windowHandle,W,H,ci.vsync);
    gx_shim_setRhi(rhi,sc); GXInit_host();

    int captured=0, frames=100000, captureFrame=40;
    for(int f=0;f<frames;++f){
        if(!plat_window_pump(win)) break;
        rhi_beginFrame(rhi);
        rhi_clear(rhi,0.10f,0.11f,0.13f,1.0f);
        float a=(float)f*0.02f, cS=cosf(a), sS=sinf(a);
        float ys=1.0f/tanf(0.9f), xs=ys/((float)W/H), zn=0.05f, zf=50.0f, dist=3.0f;
        float mv[3][4]={{cS,0,sS,0},{0,1,0,0},{-sS,0,cS,dist}};
        float proj[4][4]={{xs,0,0,0},{0,ys,0,0},{0,0,zf/(zf-zn),-zn*zf/(zf-zn)},{0,0,1,0}};
        float P[4][4]; for(int r=0;r<3;++r)for(int c=0;c<4;++c)P[r][c]=mv[r][c]; P[3][0]=P[3][1]=P[3][2]=0; P[3][3]=1;
        float mvp[16]; for(int r=0;r<4;++r)for(int c=0;c<4;++c){ float s=0; for(int k=0;k<4;++k)s+=proj[r][k]*P[k][c]; mvp[r*4+c]=s; }
        rhi_setColorTransform(rhi,mvp);
        for(int i=0;i<gNGroups;++i){ Group* g=&gGroups[i]; if(g->n==0) continue;
            if(g->haveTex){ GXLoadTexObj(&g->obj, GX_TEXMAP0); rhi_drawTextured(rhi,g->v,(uint32_t)g->n); } }
        if(gNFlat>0) rhi_drawColored(rhi,gFlat,(uint32_t)gNFlat);
        rhi_endFrame(rhi);
        rhi_present(rhi,sc);
        if(capturePath && !captured && f>=captureFrame){ sleep_ms(60); plat_window_capture_bmp(win,capturePath); printf("[model] captured -> %s\n",capturePath); captured=1; frames=f+3; }
    }

    free(tab);free(bin);free(models);free(m);
    rhi_swapchainDestroy(rhi,sc); rhi_destroy(rhi); plat_window_destroy(win); dvd_shim_shutdown();
    return 0;
}
