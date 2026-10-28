// map_view.cpp - assemble and render a whole Star Fox Adventures map from the ISO.
//
// A map (<map>/mod<NN>.zlb.bin, indexed by mod<NN>.tab) is a grid of blocks; each block
// is a MapBlockData with its own vertex pool + display lists + textures. This tool loads
// every block (via stairfax_assets), decodes its textures, and draws its display lists
// through gx_draw's indexed path, placing each block on a grid. On-disc format parsing
// lives in port/assets.h; this file is just the viewer + placement + camera.
//
// NOTE: true block adjacency comes from the level's romlist grid, which is not yet
// decoded (see [[port-map-block-format]]); blocks fall back to a square grid layout.

#include "port/gx_draw.h"
#include "port/renderer/rhi.h"
#include "port/plat_window.h"
#include "port/dvd_shim.h"
#include "port/assets.h"

#include "dolphin/gx/GXGeometry.h"
#include "dolphin/gx/GXTransform.h"
#include "dolphin/gx/GXEnum.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cstdint>
#include <cmath>
#include <vector>
#include <windows.h>

extern "C" uint8_t* gxTexDecode(int fmt, int w, int h, const uint8_t* src);

#define MAP_BLOCK_WORLD_SIZE 640.0f

struct Block { AssetMapBlock mb; std::vector<RhiTexture*> texCache; };
struct Placement { int block; float wx, wy, wz; };

static RhiBackend parseBackend(const char* s){
    if(!s) return RHI_BACKEND_D3D11;
    if(!strcmp(s,"vk")||!strcmp(s,"vulkan")) return RHI_BACKEND_VULKAN;
    if(!strcmp(s,"d3d12")) return RHI_BACKEND_D3D12;
    return RHI_BACKEND_D3D11;
}

// Program VTXFMT0..7 as videoInit() does (component types + fracs).
static void setupVtxFormats() {
    GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS,  GX_POS_XYZ,  GX_S16,   0);
    GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);
    GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_TEX0, GX_TEX_ST,   GX_S16,   7);
    GXSetVtxAttrFmt(GX_VTXFMT1, GX_VA_POS,  GX_POS_XYZ,  GX_S16,   2);
    GXSetVtxAttrFmt(GX_VTXFMT1, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);
    GXSetVtxAttrFmt(GX_VTXFMT1, GX_VA_TEX0, GX_TEX_ST,   GX_F32,   0);
    GXSetVtxAttrFmt(GX_VTXFMT2, GX_VA_POS,  GX_POS_XYZ,  GX_F32,   0);
    GXSetVtxAttrFmt(GX_VTXFMT2, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);
    GXSetVtxAttrFmt(GX_VTXFMT2, GX_VA_TEX0, GX_TEX_ST,   GX_F32,   0);
    GXSetVtxAttrFmt(GX_VTXFMT3, GX_VA_POS,  GX_POS_XYZ,  GX_S16,   8);
    GXSetVtxAttrFmt(GX_VTXFMT3, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA4, 0);
    for (int t=0;t<4;++t) GXSetVtxAttrFmt(GX_VTXFMT3, (GXAttr)(GX_VA_TEX0+t), GX_TEX_ST, GX_S16, 10);
    GXSetVtxAttrFmt(GX_VTXFMT4, GX_VA_POS,  GX_POS_XYZ,  GX_F32,   0);
    GXSetVtxAttrFmt(GX_VTXFMT4, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);
    GXSetVtxAttrFmt(GX_VTXFMT4, GX_VA_TEX0, GX_TEX_ST,   GX_S16,   7);
    GXSetVtxAttrFmt(GX_VTXFMT5, GX_VA_POS,  GX_POS_XYZ,  GX_S16,   3);
    GXSetVtxAttrFmt(GX_VTXFMT5, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA4, 0);
    for (int t=0;t<4;++t) GXSetVtxAttrFmt(GX_VTXFMT5, (GXAttr)(GX_VA_TEX0+t), GX_TEX_ST, GX_S16, 8);
    GXSetVtxAttrFmt(GX_VTXFMT6, GX_VA_POS,  GX_POS_XYZ,  GX_S16,   8);
    GXSetVtxAttrFmt(GX_VTXFMT6, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA4, 0);
    for (int t=0;t<4;++t) GXSetVtxAttrFmt(GX_VTXFMT6, (GXAttr)(GX_VA_TEX0+t), GX_TEX_ST, GX_S16, 10);
    GXSetVtxAttrFmt(GX_VTXFMT7, GX_VA_POS,  GX_POS_XYZ,  GX_S16,   0);
    GXSetVtxAttrFmt(GX_VTXFMT7, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA4, 0);
    for (int t=0;t<4;++t) GXSetVtxAttrFmt(GX_VTXFMT7, (GXAttr)(GX_VA_TEX0+t), GX_TEX_ST, GX_S16, 10);
}

int main(int argc, char** argv) {
    const char* iso=nullptr; const char* dir="desert"; int mod=29;
    const char* gfx="d3d11"; const char* capturePath=nullptr; int frames=300, captureFrame=40; int oneBlock=-1;
    for (int i=1;i<argc;++i){
        if      (!strcmp(argv[i],"--iso")&&i+1<argc) iso=argv[++i];
        else if (!strcmp(argv[i],"--dir")&&i+1<argc) dir=argv[++i];
        else if (!strcmp(argv[i],"--mod")&&i+1<argc) mod=atoi(argv[++i]);
        else if (!strcmp(argv[i],"--block")&&i+1<argc) oneBlock=atoi(argv[++i]);
        else if (!strcmp(argv[i],"--gfx")&&i+1<argc) gfx=argv[++i];
        else if (!strcmp(argv[i],"--capture")&&i+1<argc) capturePath=argv[++i];
        else if (!strcmp(argv[i],"--frames")&&i+1<argc) frames=atoi(argv[++i]);
    }
    if(!iso){ fprintf(stderr,"usage: map_view --iso <path> [--dir desert --mod 29 --block N]\n"); return 2; }

    dvd_shim_init(iso);
    char tabPath[256], binPath[256];
    snprintf(tabPath,sizeof tabPath,"%s/mod%d.tab",dir,mod);
    snprintf(binPath,sizeof binPath,"%s/mod%d.zlb.bin",dir,mod);
    int tabSize=0, binSize=0;
    unsigned char* tab=(unsigned char*)loadFileByPath(tabPath,&tabSize,0);
    unsigned char* bin=(unsigned char*)loadFileByPath(binPath,&binSize,0);
    if(!tab||!bin){ fprintf(stderr,"[map] cannot read %s / %s\n",tabPath,binPath); return 3; }

    // Load every valid, distinct block (or just one with --block).
    std::vector<Block> blocks;
    unsigned lastOff=0xffffffff;
    for (int i=0;i<tabSize/4;++i){
        if (oneBlock>=0 && i!=oneBlock) continue;
        unsigned off=asset_mapBlockOffset(tab,tabSize,i); if(off==ASSET_NO_BLOCK||off==lastOff) continue; lastOff=off;
        Block b; if(asset_loadMapBlock(bin,binSize,tab,tabSize,i,&b.mb)) blocks.push_back(std::move(b));
    }
    if(blocks.empty()){ fprintf(stderr,"[map] no valid blocks\n"); return 4; }
    printf("[map] %s mod%d: %zu blocks loaded\n", dir, mod, blocks.size());

    // Placement: true adjacency needs the romlist grid (unsolved) - lay blocks in a
    // square grid so all of them are visible, contiguous tiles at their real elevations.
    std::vector<Placement> places;
    int cols=(int)ceilf(sqrtf((float)blocks.size()));
    for (int i=0;i<(int)blocks.size();++i){ Placement pl; pl.block=i;
        pl.wx=(i%cols)*MAP_BLOCK_WORLD_SIZE; pl.wz=(i/cols)*MAP_BLOCK_WORLD_SIZE; pl.wy=(float)blocks[i].mb.yOff;
        places.push_back(pl); }

    // World bounding box over placed blocks (local bbox + world translation).
    float wmn[3]={1e18f,1e18f,1e18f}, wmx[3]={-1e18f,-1e18f,-1e18f};
    for (auto& pl : places) {
        AssetMapBlock& b=blocks[pl.block].mb; const uint8_t* d=b.data;
        float scl=1.0f/(float)(1<<ASSET_POS_FRAC[b.fmt&7]); float t[3]={pl.wx,pl.wy,pl.wz};
        for (int v=0;v<b.vertCount;++v){ const uint8_t* vp=d+b.vertOff+v*6;
            float w[3]={asset_s16be(vp)*scl+t[0], asset_s16be(vp+2)*scl+t[1], asset_s16be(vp+4)*scl+t[2]};
            for(int k=0;k<3;++k){ if(w[k]<wmn[k])wmn[k]=w[k]; if(w[k]>wmx[k])wmx[k]=w[k]; } }
    }
    float cx=(wmn[0]+wmx[0])*0.5f, cy=(wmn[1]+wmx[1])*0.5f, cz=(wmn[2]+wmx[2])*0.5f;
    float R=1; for(int k=0;k<3;++k){ float e=(wmx[k]-wmn[k])*0.5f; if(e>R)R=e; }
    float dist=R*2.0f;
    printf("[map] world center=(%.0f,%.0f,%.0f) R=%.0f\n",cx,cy,cz,R);

    const int W=1280,H=720;
    char title[160]; snprintf(title,sizeof title,"Stairfax Temperatures - %s map [%s]",dir,gfx);
    PlatWindow* win=plat_window_create(title,W,H); if(!win) return 5;
    RhiCreateInfo ci={}; ci.backend=parseBackend(gfx); ci.windowHandle=plat_window_native_handle(win);
    ci.width=W; ci.height=H; ci.vsync=true; ci.appName="Stairfax Temperatures";
    RhiInstance* rhi=rhi_create(&ci); if(!rhi) return 6;
    RhiSwapchain* sc=rhi_swapchainCreate(rhi,ci.windowHandle,W,H,ci.vsync);
    gx_draw_init(); gx_draw_setRhi(rhi,sc); setupVtxFormats();
    printf("[map] backend=%s\n", rhi_backendName(rhi_getBackend(rhi)));

    // Per-block textures from the map's TEX1 (shader.layers[0].textureIndex -> block
    // textures[] value v -> TEX1.tab[v], ZLB or DIR wrapper).
    char p1t[256],p1b[256]; snprintf(p1t,256,"%s/TEX1.tab",dir); snprintf(p1b,256,"%s/TEX1.bin",dir);
    int s1t=0,s1b=0; unsigned char* t1tab=(unsigned char*)loadFileByPath(p1t,&s1t,0);
    unsigned char* t1bin=(unsigned char*)loadFileByPath(p1b,&s1b,0);
    int texOK=0,texTot=0;
    if (t1tab&&t1bin){ static unsigned char tbuf[4*1024*1024];
        for (auto& blk : blocks){ AssetMapBlock& b=blk.mb; blk.texCache.assign(b.texCount,nullptr);
            for (int k=0;k<b.texCount;++k){ int id=(int)(asset_be32(b.data+b.texArrOff+k*4)&0x7fff);
                int w,h,fmt; unsigned io; texTot++;
                if (asset_loadTexRecord(t1tab,s1t,t1bin,s1b,id,tbuf,sizeof tbuf,&w,&h,&fmt,&io)){
                    uint8_t* rgba=gxTexDecode(fmt,w,h,tbuf+io);
                    if(rgba){ blk.texCache[k]=rhi_createTexture(rhi,w,h,1,(uint32_t)fmt,rgba); free(rgba); if(blk.texCache[k])texOK++; } } } }
    }
    printf("[map] textures: %d/%d decoded from TEX1\n", texOK, texTot);

    int captured=0;
    for (int f=0; f<frames; ++f) {
        if(!plat_window_pump(win)) break;
        rhi_beginFrame(rhi);
        rhi_clear(rhi, 0.06f, 0.07f, 0.10f, 1.0f);

        float proj[4][4]; memset(proj,0,sizeof proj);
        float ys=1.0f/tanf((55.0f*3.14159265f/180.0f)*0.5f);
        float zn=fmaxf(R*0.02f,1.0f), zf=dist+4.0f*R+1.0f;
        proj[0][0]=ys/((float)W/H); proj[1][1]=ys; proj[2][2]=zf/(zf-zn);
        proj[2][3]=-zn*zf/(zf-zn); proj[3][2]=1.0f;
        GXSetProjection(proj, GX_PERSPECTIVE);

        float a=(float)f*0.008f, cyw=cosf(a), sy=sinf(a);
        float pitch=0.62f, cp=cosf(pitch), sp=sinf(pitch);
        float Ry[3][3]={{cyw,0,sy},{0,1,0},{-sy,0,cyw}};
        float Rx[3][3]={{1,0,0},{0,cp,-sp},{0,sp,cp}};
        float Rot[3][3];
        for(int r=0;r<3;++r)for(int c=0;c<3;++c){float s2=0;for(int k=0;k<3;++k)s2+=Rx[r][k]*Ry[k][c];Rot[r][c]=s2;}
        float ctr[3]={cx,cy,cz}, camView[3][4];
        for(int r=0;r<3;++r){ float t=0; for(int k=0;k<3;++k){ camView[r][k]=Rot[r][k]; t-=Rot[r][k]*ctr[k]; } camView[r][3]=t; }
        camView[2][3]+=dist;

        for (auto& pl : places) {
            Block& blk=blocks[pl.block]; AssetMapBlock& b=blk.mb; const uint8_t* d=b.data;
            GXClearVtxDesc();
            GXSetVtxDesc(GX_VA_POS,  b.posSz==2?GX_INDEX16:GX_INDEX8);
            GXSetVtxDesc(GX_VA_CLR0, b.clrSz==2?GX_INDEX16:GX_INDEX8);
            GXSetVtxDesc(GX_VA_TEX0, b.texSz==2?GX_INDEX16:GX_INDEX8);
            GXSetArray(GX_VA_POS, (void*)(d+b.vertOff), 6);
            if(b.colOff) GXSetArray(GX_VA_CLR0, (void*)(d+b.colOff), 2);
            if(b.texOff) GXSetArray(GX_VA_TEX0, (void*)(d+b.texOff), 4);

            float mv[3][4];
            for(int r=0;r<3;++r){ mv[r][0]=camView[r][0]; mv[r][1]=camView[r][1]; mv[r][2]=camView[r][2];
                mv[r][3]=camView[r][0]*pl.wx+camView[r][1]*pl.wy+camView[r][2]*pl.wz+camView[r][3]; }
            GXLoadPosMtxImm(mv, GX_PNMTX0);
            GXSetCurrentMtx(GX_PNMTX0);
            gx_draw_setSourceBounds(d + b.vertOff, d + b.vertOff + (size_t)b.vertCount * 6);

            for (int i=0;i<b.dlCount;++i){ const uint8_t* rec=d+b.dlBase+i*0x1C;
                unsigned o=asset_be32(rec); int sz=asset_be16(rec+4); int shIdx=rec[0x13];
                RhiTexture* tex=nullptr;
                if(shIdx<b.shCount){ int ti=(int)asset_be32(d+b.shOff+shIdx*0x44+0x24);
                    if(ti>=0 && ti<(int)blk.texCache.size()) tex=blk.texCache[ti]; }
                gx_draw_setTexture(tex);
                if(sz>0 && o+(unsigned)sz<=b.size) GXCallDisplayList((void*)(d+o), (unsigned)sz);
            }
        }

        rhi_endFrame(rhi); rhi_present(rhi,sc);
        if(capturePath && !captured && f>=captureFrame){ Sleep(60);
            if(plat_window_capture_bmp(win,capturePath)) printf("[map] captured frame %d -> %s\n",f,capturePath);
            captured=1; frames=f+3;
        }
    }
    for (auto& blk : blocks) asset_freeMapBlock(&blk.mb);
    rhi_swapchainDestroy(rhi,sc); rhi_destroy(rhi); plat_window_destroy(win);
    printf("[map] done\n");
    return 0;
}
