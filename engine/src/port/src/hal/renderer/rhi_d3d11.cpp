// rhi_d3d11.cpp - D3D11 backend (best-effort). Clear-to-color bring-up.
//
// C++ TU exposing a C ABI (rhi_d3d11_available / rhi_d3d11_create). COM via ComPtr.
// Feature level 11_0/11_1, flip-model swapchain, immediate-context clear.

#include "port/renderer/rhi_d3d11.h"
#include "port/renderer/rhi_internal.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

namespace {

struct D3d11Swapchain {
    ComPtr<IDXGISwapChain1>        swap;
    ComPtr<ID3D11RenderTargetView> rtv;
    ComPtr<ID3D11Texture2D>        depthTex;
    ComPtr<ID3D11DepthStencilView> dsv;
    int  w = 0, h = 0;
    bool vsync = true;
};

struct D3d11Texture {
    ComPtr<ID3D11Texture2D>          tex;
    ComPtr<ID3D11ShaderResourceView> srv;
};

struct D3d11Instance {
    RhiInstance                 base;
    ComPtr<ID3D11Device>        device;
    ComPtr<ID3D11DeviceContext> ctx;
    D3d11Swapchain*             active = nullptr;

    // Textured pipeline (pos+color+uv, samples t0 * vertex color).
    ComPtr<ID3D11VertexShader>   vsTex;
    ComPtr<ID3D11PixelShader>    psTex;
    ComPtr<ID3D11InputLayout>    layoutTex;
    ComPtr<ID3D11SamplerState>   samp;
    ID3D11ShaderResourceView*    curSRV = nullptr; // non-owning, currently bound
    bool                         texPipelineReady = false;

    // Built-in position+color pipeline (created lazily on first drawColored).
    ComPtr<ID3D11VertexShader>   vs;
    ComPtr<ID3D11PixelShader>    ps;
    ComPtr<ID3D11InputLayout>    layout;
    ComPtr<ID3D11BlendState>     blend;
    ComPtr<ID3D11BlendState>     blendAlpha; // src-over for RHI_ALPHA_BLEND
    ComPtr<ID3D11RasterizerState> raster;
    ComPtr<ID3D11DepthStencilState> depthOff;
    ComPtr<ID3D11DepthStencilState> depthOn;
    ComPtr<ID3D11DepthStencilState> depthReadOnly; // test on, write off (blended geometry)
    ComPtr<ID3D11Buffer>         dynVB;
    UINT                         dynVBCap = 0;
    ComPtr<ID3D11Buffer>         cbXform; // 4x4 MVP (row-major)
    ComPtr<ID3D11Buffer>         cbAlpha; // x = alpha-test ref (<0 disables the clip)
    float                        mvp[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
    bool                         pipelineReady = false;
};

D3d11Instance* self(RhiInstance* r) { return reinterpret_cast<D3d11Instance*>(r); }

bool makeRTV(D3d11Instance* s, D3d11Swapchain* sc) {
    ComPtr<ID3D11Texture2D> backbuf;
    if (FAILED(sc->swap->GetBuffer(0, IID_PPV_ARGS(&backbuf)))) return false;
    if (FAILED(s->device->CreateRenderTargetView(backbuf.Get(), nullptr, &sc->rtv))) return false;

    // Depth buffer matching the swapchain.
    sc->dsv.Reset(); sc->depthTex.Reset();
    D3D11_TEXTURE2D_DESC dd = {};
    dd.Width = (UINT)sc->w; dd.Height = (UINT)sc->h; dd.MipLevels = 1; dd.ArraySize = 1;
    dd.Format = DXGI_FORMAT_D32_FLOAT; dd.SampleDesc.Count = 1;
    dd.Usage = D3D11_USAGE_DEFAULT; dd.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    if (FAILED(s->device->CreateTexture2D(&dd, nullptr, &sc->depthTex))) return false;
    if (FAILED(s->device->CreateDepthStencilView(sc->depthTex.Get(), nullptr, &sc->dsv))) return false;
    return true;
}

RhiSwapchain* d3d11_swapchainCreate(RhiInstance* r, void* windowHandle, int w, int h, bool vsync) {
    D3d11Instance* s = self(r);
    D3d11Swapchain* sc = new D3d11Swapchain();
    sc->w = w; sc->h = h; sc->vsync = vsync;

    ComPtr<IDXGIDevice>  dxgiDev;
    ComPtr<IDXGIAdapter> adapter;
    ComPtr<IDXGIFactory2> factory;
    if (FAILED(s->device.As(&dxgiDev)))              { delete sc; return nullptr; }
    if (FAILED(dxgiDev->GetAdapter(&adapter)))       { delete sc; return nullptr; }
    if (FAILED(adapter->GetParent(IID_PPV_ARGS(&factory)))) { delete sc; return nullptr; }

    DXGI_SWAP_CHAIN_DESC1 desc = {};
    desc.Width  = (UINT)w;
    desc.Height = (UINT)h;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 2;
    desc.SwapEffect  = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    desc.AlphaMode   = DXGI_ALPHA_MODE_IGNORE;

    if (FAILED(factory->CreateSwapChainForHwnd(
            s->device.Get(), (HWND)windowHandle, &desc, nullptr, nullptr, &sc->swap))) {
        delete sc; return nullptr;
    }
    if (!makeRTV(s, sc)) { delete sc; return nullptr; }

    s->active = sc;
    return reinterpret_cast<RhiSwapchain*>(sc);
}

void d3d11_swapchainDestroy(RhiInstance* r, RhiSwapchain* h) {
    D3d11Instance* s = self(r);
    D3d11Swapchain* sc = reinterpret_cast<D3d11Swapchain*>(h);
    if (s->active == sc) s->active = nullptr;
    delete sc;
}

void d3d11_swapchainResize(RhiInstance* r, RhiSwapchain* h, int w, int t) {
    D3d11Instance* s = self(r);
    D3d11Swapchain* sc = reinterpret_cast<D3d11Swapchain*>(h);
    if (!sc || !sc->swap) return;
    s->ctx->OMSetRenderTargets(0, nullptr, nullptr);
    sc->rtv.Reset();
    sc->swap->ResizeBuffers(0, (UINT)w, (UINT)t, DXGI_FORMAT_UNKNOWN, 0);
    sc->w = w; sc->h = t;
    makeRTV(s, sc);
}

bool d3d11_present(RhiInstance* r, RhiSwapchain* h) {
    (void)r;
    D3d11Swapchain* sc = reinterpret_cast<D3d11Swapchain*>(h);
    if (!sc || !sc->swap) return false;
    return SUCCEEDED(sc->swap->Present(sc->vsync ? 1 : 0, 0));
}

void d3d11_beginFrame(RhiInstance* r) {
    D3d11Instance* s = self(r);
    if (!s->active || !s->active->rtv) return;
    ID3D11RenderTargetView* rtv = s->active->rtv.Get();
    s->ctx->OMSetRenderTargets(1, &rtv, s->active->dsv.Get());
    if (s->active->dsv) s->ctx->ClearDepthStencilView(s->active->dsv.Get(), D3D11_CLEAR_DEPTH, 1.0f, 0);
    D3D11_VIEWPORT vp = {};
    vp.Width  = (FLOAT)s->active->w;
    vp.Height = (FLOAT)s->active->h;
    vp.MaxDepth = 1.0f;
    s->ctx->RSSetViewports(1, &vp);
}

void d3d11_endFrame(RhiInstance* r) { (void)r; }

void d3d11_clear(RhiInstance* r, float cr, float cg, float cb, float ca) {
    D3d11Instance* s = self(r);
    if (!s->active || !s->active->rtv) return;
    const float color[4] = { cr, cg, cb, ca };
    s->ctx->ClearRenderTargetView(s->active->rtv.Get(), color);
}

static const char* kColorHLSL =
    "cbuffer Xform : register(b0) { float4 uRows[4]; };\n"
    "struct VSIn  { float3 pos : POSITION; float4 col : COLOR; };\n"
    "struct VSOut { float4 pos : SV_Position; float4 col : COLOR; };\n"
    "VSOut vsmain(VSIn i){ float4 p = float4(i.pos, 1.0); VSOut o;\n"
    "  o.pos = float4(dot(uRows[0],p), dot(uRows[1],p), dot(uRows[2],p), dot(uRows[3],p));\n"
    "  o.col = i.col; return o; }\n"
    "float4 psmain(VSOut i) : SV_Target { return i.col; }\n";

bool buildPipeline(D3d11Instance* s) {
    if (s->pipelineReady) return true;

    ComPtr<ID3DBlob> vsb, psb, err;
    if (FAILED(D3DCompile(kColorHLSL, strlen(kColorHLSL), "color", nullptr, nullptr,
                          "vsmain", "vs_4_0", 0, 0, &vsb, &err))) return false;
    if (FAILED(D3DCompile(kColorHLSL, strlen(kColorHLSL), "color", nullptr, nullptr,
                          "psmain", "ps_4_0", 0, 0, &psb, &err))) return false;
    if (FAILED(s->device->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &s->vs))) return false;
    if (FAILED(s->device->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &s->ps))) return false;

    D3D11_INPUT_ELEMENT_DESC elems[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,  D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "COLOR",    0, DXGI_FORMAT_R8G8B8A8_UNORM,  0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0 },
    };
    if (FAILED(s->device->CreateInputLayout(elems, 2, vsb->GetBufferPointer(), vsb->GetBufferSize(), &s->layout))) return false;

    D3D11_BLEND_DESC bd = {};
    bd.RenderTarget[0].BlendEnable = FALSE;
    bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    s->device->CreateBlendState(&bd, &s->blend);

    D3D11_BLEND_DESC ba = {};
    ba.RenderTarget[0].BlendEnable = TRUE;
    ba.RenderTarget[0].SrcBlend  = D3D11_BLEND_SRC_ALPHA;
    ba.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    ba.RenderTarget[0].BlendOp   = D3D11_BLEND_OP_ADD;
    ba.RenderTarget[0].SrcBlendAlpha  = D3D11_BLEND_ONE;
    ba.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    ba.RenderTarget[0].BlendOpAlpha   = D3D11_BLEND_OP_ADD;
    ba.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    s->device->CreateBlendState(&ba, &s->blendAlpha);

    D3D11_RASTERIZER_DESC rd = {};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;   // GX cull handled later; draw everything for now
    s->device->CreateRasterizerState(&rd, &s->raster);

    D3D11_DEPTH_STENCIL_DESC dd = {};
    dd.DepthEnable = FALSE;
    s->device->CreateDepthStencilState(&dd, &s->depthOff);
    D3D11_DEPTH_STENCIL_DESC don = {};
    don.DepthEnable = TRUE;
    don.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    don.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
    s->device->CreateDepthStencilState(&don, &s->depthOn);
    D3D11_DEPTH_STENCIL_DESC dro = don;
    dro.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;   // blended: test but don't occlude
    s->device->CreateDepthStencilState(&dro, &s->depthReadOnly);

    D3D11_BUFFER_DESC cbd = {};
    cbd.ByteWidth = 16 * sizeof(float);
    cbd.Usage = D3D11_USAGE_DYNAMIC;
    cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    s->device->CreateBuffer(&cbd, nullptr, &s->cbXform);

    D3D11_BUFFER_DESC cba = {};
    cba.ByteWidth = 4 * sizeof(float);   // float4 (only .x used)
    cba.Usage = D3D11_USAGE_DYNAMIC;
    cba.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cba.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    s->device->CreateBuffer(&cba, nullptr, &s->cbAlpha);

    s->pipelineReady = true;
    return true;
}

void d3d11_setColorTransform(RhiInstance* r, const float m[16]) {
    memcpy(self(r)->mvp, m, 16 * sizeof(float));
}

static const char* kTexHLSL =
    "cbuffer Xform : register(b0) { float4 uRows[4]; };\n"
    "cbuffer Alpha : register(b1) { float4 uAlpha; };\n"  // .x = alpha-test ref (<0 = off)
    "Texture2D uTex : register(t0);\n"
    "SamplerState uSamp : register(s0);\n"
    "struct VSIn  { float3 pos : POSITION; float4 col : COLOR; float2 uv : TEXCOORD; };\n"
    "struct VSOut { float4 pos : SV_Position; float4 col : COLOR; float2 uv : TEXCOORD; };\n"
    "VSOut vsmain(VSIn i){ float4 p = float4(i.pos,1.0); VSOut o;\n"
    "  o.pos = float4(dot(uRows[0],p), dot(uRows[1],p), dot(uRows[2],p), dot(uRows[3],p));\n"
    "  o.col = i.col; o.uv = i.uv; return o; }\n"
    "float4 psmain(VSOut i) : SV_Target {\n"
    "  float4 c = i.col * uTex.Sample(uSamp, i.uv);\n"
    "  clip(c.a - uAlpha.x);\n"   // uAlpha.x<0 -> never clips; >0 -> discard below cutout
    "  return c; }\n";

bool buildTexPipeline(D3d11Instance* s) {
    if (s->texPipelineReady) return true;
    if (!buildPipeline(s)) return false; // ensures cbXform, blend, raster, depthOff exist

    ComPtr<ID3DBlob> vsb, psb, err;
    if (FAILED(D3DCompile(kTexHLSL, strlen(kTexHLSL), "tex", nullptr, nullptr, "vsmain", "vs_4_0", 0, 0, &vsb, &err))) return false;
    if (FAILED(D3DCompile(kTexHLSL, strlen(kTexHLSL), "tex", nullptr, nullptr, "psmain", "ps_4_0", 0, 0, &psb, &err))) return false;
    if (FAILED(s->device->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &s->vsTex))) return false;
    if (FAILED(s->device->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &s->psTex))) return false;

    D3D11_INPUT_ELEMENT_DESC elems[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,  D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "COLOR",    0, DXGI_FORMAT_R8G8B8A8_UNORM,  0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,    0, 16, D3D11_INPUT_PER_VERTEX_DATA, 0 },
    };
    if (FAILED(s->device->CreateInputLayout(elems, 3, vsb->GetBufferPointer(), vsb->GetBufferSize(), &s->layoutTex))) return false;

    D3D11_SAMPLER_DESC sd = {};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    s->device->CreateSamplerState(&sd, &s->samp);

    s->texPipelineReady = true;
    return true;
}

RhiTexture* d3d11_createTexture(RhiInstance* r, int w, int h, int mips, uint32_t fmt, const void* rgba8) {
    (void)mips; (void)fmt;
    D3d11Instance* s = self(r);
    if (w <= 0 || h <= 0) return nullptr;
    D3d11Texture* t = new D3d11Texture();
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = (UINT)w; td.Height = (UINT)h; td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_IMMUTABLE;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA sub = {};
    sub.pSysMem = rgba8;
    sub.SysMemPitch = (UINT)w * 4;
    if (FAILED(s->device->CreateTexture2D(&td, &sub, &t->tex))) { delete t; return nullptr; }
    if (FAILED(s->device->CreateShaderResourceView(t->tex.Get(), nullptr, &t->srv))) { delete t; return nullptr; }
    return reinterpret_cast<RhiTexture*>(t);
}

void d3d11_destroyTexture(RhiInstance* r, RhiTexture* h) {
    (void)r;
    delete reinterpret_cast<D3d11Texture*>(h);
}

void d3d11_setTexture(RhiInstance* r, int slot, RhiTexture* h) {
    (void)slot;
    D3d11Instance* s = self(r);
    D3d11Texture* t = reinterpret_cast<D3d11Texture*>(h);
    s->curSRV = t ? t->srv.Get() : nullptr;
}

void d3d11_drawTextured(RhiInstance* r, const RhiTexVertex* verts, uint32_t count) {
    D3d11Instance* s = self(r);
    if (!verts || count == 0 || !s->active) return;
    if (!buildTexPipeline(s)) return;

    const UINT stride = (UINT)sizeof(RhiTexVertex);
    const UINT needed = stride * count;
    if (needed > s->dynVBCap) {
        s->dynVB.Reset();
        D3D11_BUFFER_DESC bd = {};
        bd.ByteWidth = needed; bd.Usage = D3D11_USAGE_DYNAMIC;
        bd.BindFlags = D3D11_BIND_VERTEX_BUFFER; bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(s->device->CreateBuffer(&bd, nullptr, &s->dynVB))) return;
        s->dynVBCap = needed;
    }
    D3D11_MAPPED_SUBRESOURCE mapped;
    if (FAILED(s->ctx->Map(s->dynVB.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) return;
    memcpy(mapped.pData, verts, needed);
    s->ctx->Unmap(s->dynVB.Get(), 0);

    D3D11_MAPPED_SUBRESOURCE cbm;
    if (SUCCEEDED(s->ctx->Map(s->cbXform.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &cbm))) {
        memcpy(cbm.pData, s->mvp, 16 * sizeof(float));
        s->ctx->Unmap(s->cbXform.Get(), 0);
    }

    // Alpha handling: TEST clips texels below a cutout; BLEND does src-over without
    // depth writes; OPAQUE ignores texel alpha. See RhiAlphaMode.
    int mode = r->alphaMode;
    float alphaRef = (mode == RHI_ALPHA_TEST) ? 0.5f : -1.0f;
    D3D11_MAPPED_SUBRESOURCE abm;
    if (SUCCEEDED(s->ctx->Map(s->cbAlpha.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &abm))) {
        float a4[4] = { alphaRef, 0, 0, 0 };
        memcpy(abm.pData, a4, sizeof(a4));
        s->ctx->Unmap(s->cbAlpha.Get(), 0);
    }

    ID3D11Buffer* vb = s->dynVB.Get();
    ID3D11Buffer* cb = s->cbXform.Get();
    ID3D11Buffer* cba = s->cbAlpha.Get();
    ID3D11SamplerState* samp = s->samp.Get();
    UINT offset = 0;
    s->ctx->IASetInputLayout(s->layoutTex.Get());
    s->ctx->IASetVertexBuffers(0, 1, &vb, &stride, &offset);
    s->ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    s->ctx->VSSetConstantBuffers(0, 1, &cb);
    s->ctx->VSSetShader(s->vsTex.Get(), nullptr, 0);
    s->ctx->PSSetShader(s->psTex.Get(), nullptr, 0);
    s->ctx->PSSetConstantBuffers(1, 1, &cba);
    s->ctx->PSSetShaderResources(0, 1, &s->curSRV);
    s->ctx->PSSetSamplers(0, 1, &samp);
    const float bf[4] = {0,0,0,0};
    s->ctx->OMSetBlendState(mode == RHI_ALPHA_BLEND ? s->blendAlpha.Get() : s->blend.Get(), bf, 0xffffffff);
    s->ctx->OMSetDepthStencilState(mode == RHI_ALPHA_BLEND ? s->depthReadOnly.Get() : s->depthOn.Get(), 0);
    s->ctx->RSSetState(s->raster.Get());
    s->ctx->Draw(count, 0);
}

void d3d11_drawColored(RhiInstance* r, const RhiColorVertex* verts, uint32_t count) {
    D3d11Instance* s = self(r);
    if (!verts || count == 0 || !s->active) return;
    if (!buildPipeline(s)) return;

    const UINT stride = (UINT)sizeof(RhiColorVertex);
    const UINT needed = stride * count;
    if (needed > s->dynVBCap) {
        s->dynVB.Reset();
        D3D11_BUFFER_DESC bd = {};
        bd.ByteWidth = needed;
        bd.Usage = D3D11_USAGE_DYNAMIC;
        bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(s->device->CreateBuffer(&bd, nullptr, &s->dynVB))) return;
        s->dynVBCap = needed;
    }

    D3D11_MAPPED_SUBRESOURCE mapped;
    if (FAILED(s->ctx->Map(s->dynVB.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) return;
    memcpy(mapped.pData, verts, needed);
    s->ctx->Unmap(s->dynVB.Get(), 0);

    D3D11_MAPPED_SUBRESOURCE cbm;
    if (SUCCEEDED(s->ctx->Map(s->cbXform.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &cbm))) {
        memcpy(cbm.pData, s->mvp, 16 * sizeof(float));
        s->ctx->Unmap(s->cbXform.Get(), 0);
    }

    ID3D11Buffer* vb = s->dynVB.Get();
    ID3D11Buffer* cb = s->cbXform.Get();
    UINT offset = 0;
    s->ctx->IASetInputLayout(s->layout.Get());
    s->ctx->IASetVertexBuffers(0, 1, &vb, &stride, &offset);
    s->ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    s->ctx->VSSetConstantBuffers(0, 1, &cb);
    s->ctx->VSSetShader(s->vs.Get(), nullptr, 0);
    s->ctx->PSSetShader(s->ps.Get(), nullptr, 0);
    const float bf[4] = {0,0,0,0};
    s->ctx->OMSetBlendState(s->blend.Get(), bf, 0xffffffff);
    s->ctx->OMSetDepthStencilState(s->depthOn.Get(), 0);
    s->ctx->RSSetState(s->raster.Get());
    s->ctx->Draw(count, 0);
}

void d3d11_destroy(RhiInstance* r) {
    D3d11Instance* s = self(r);
    if (!s) return;
    if (s->ctx) s->ctx->ClearState();
    delete s->active;
    delete s;
}

const RhiOps kOps = {
    d3d11_destroy,
    d3d11_swapchainCreate,
    d3d11_swapchainDestroy,
    d3d11_swapchainResize,
    d3d11_present,
    d3d11_beginFrame,
    d3d11_endFrame,
    d3d11_clear,
    d3d11_drawColored,
    d3d11_setColorTransform,
    d3d11_createTexture,
    d3d11_destroyTexture,
    d3d11_setTexture,
    d3d11_drawTextured,
};

} // namespace

extern "C" bool rhi_d3d11_available(void) {
    return true; // D3D11 ships with the OS; device creation is the real gate.
}

extern "C" RhiInstance* rhi_d3d11_create(const RhiCreateInfo* info) {
    D3d11Instance* s = new D3d11Instance();
    s->base.ops     = &kOps;
    s->base.backend = RHI_BACKEND_D3D11;

    UINT flags = 0;
    if (info->debug) flags |= D3D11_CREATE_DEVICE_DEBUG;
    const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };

    HRESULT hr = D3D11CreateDevice(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
        levels, (UINT)(sizeof(levels) / sizeof(levels[0])),
        D3D11_SDK_VERSION, &s->device, nullptr, &s->ctx);

    // Retry without the debug layer if it isn't installed.
    if (FAILED(hr) && (flags & D3D11_CREATE_DEVICE_DEBUG)) {
        flags &= ~D3D11_CREATE_DEVICE_DEBUG;
        hr = D3D11CreateDevice(
            nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
            levels, (UINT)(sizeof(levels) / sizeof(levels[0])),
            D3D11_SDK_VERSION, &s->device, nullptr, &s->ctx);
    }
    if (FAILED(hr)) { delete s; return nullptr; }

    // Swapchain is created explicitly via rhi_swapchainCreate (VI owns it);
    // an HWND can host only one flip-model swapchain at a time.
    (void)info;
    return &s->base;
}
