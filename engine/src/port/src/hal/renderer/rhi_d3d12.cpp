// rhi_d3d12.cpp - D3D12 backend (primary on Windows). Clear-to-color bring-up.
//
// C++ TU exposing a C ABI. Classic command-queue/descriptor-heap/fence setup.
// Sync is a full GPU flush per present - not the final perf model, but correct and
// simple for bring-up; the frame pipeline gets deepened when real drawing lands.

#include "port/renderer/rhi_d3d12.h"
#include "port/renderer/rhi_internal.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

namespace {

const UINT kFrameCount = 2;

struct D3d12Swapchain {
    ComPtr<IDXGISwapChain3>      swap;
    ComPtr<ID3D12DescriptorHeap> rtvHeap;
    ComPtr<ID3D12Resource>       renderTargets[kFrameCount];
    ComPtr<ID3D12CommandAllocator> allocators[kFrameCount];
    UINT   rtvDescSize = 0;
    UINT   frameIndex  = 0;
    int    w = 0, h = 0;
    bool   vsync = true;

    ComPtr<ID3D12Fence> fence;
    UINT64              fenceValue = 0;
    HANDLE              fenceEvent = nullptr;

    ComPtr<ID3D12Resource>       depth;
    ComPtr<ID3D12DescriptorHeap> dsvHeap;
};

struct D3d12Texture {
    ComPtr<ID3D12Resource>      res;
    D3D12_GPU_DESCRIPTOR_HANDLE gpu = {};
};

struct D3d12Instance {
    RhiInstance                        base;
    ComPtr<IDXGIFactory4>              factory;
    ComPtr<ID3D12Device>               device;
    ComPtr<ID3D12CommandQueue>         queue;
    ComPtr<ID3D12GraphicsCommandList>  cmdList;
    D3d12Swapchain*                    active = nullptr;

    // Built-in position+color pipeline (created lazily on first drawColored).
    ComPtr<ID3D12RootSignature> rootSig;
    ComPtr<ID3D12PipelineState> pso;
    ComPtr<ID3D12Resource>      uploadVB;   // persistently-mapped upload heap (per-frame ring)
    void*                       mappedVB = nullptr;
    UINT                        uploadCap = 0;      // ring size in bytes
    UINT                        uploadOffset = 0;   // bump pointer within the ring this frame
    UINT                        uploadWant = 0;     // high-water bytes; grow at next beginFrame
    float                       mvp[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
    bool                        pipelineReady = false;

    // Textured pipeline + SRV heap.
    ComPtr<ID3D12RootSignature>  rootSigTex;
    ComPtr<ID3D12PipelineState>  psoTex;       // opaque / alpha-test (depth write on, blend off)
    ComPtr<ID3D12PipelineState>  psoTexBlend;  // src-over blend, depth write off
    ComPtr<ID3D12DescriptorHeap> srvHeap;  // shader-visible CBV_SRV_UAV
    UINT                         srvDescSize = 0;
    UINT                         srvNext = 0;
    bool                         texPipelineReady = false;
    D3D12_GPU_DESCRIPTOR_HANDLE  curTexGpu = {};
    bool                         curTexValid = false;
};

D3d12Instance* self(RhiInstance* r) { return reinterpret_cast<D3d12Instance*>(r); }

D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle(D3d12Swapchain* sc) {
    D3D12_CPU_DESCRIPTOR_HANDLE h = sc->rtvHeap->GetCPUDescriptorHandleForHeapStart();
    h.ptr += (SIZE_T)sc->frameIndex * sc->rtvDescSize;
    return h;
}

void barrier(ID3D12GraphicsCommandList* cl, ID3D12Resource* res,
             D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
    D3D12_RESOURCE_BARRIER b = {};
    b.Type  = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    b.Transition.pResource   = res;
    b.Transition.StateBefore = before;
    b.Transition.StateAfter  = after;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    cl->ResourceBarrier(1, &b);
}

void waitIdle(D3d12Instance* s) {
    if (!s->active || !s->active->fence) return;
    D3d12Swapchain* sc = s->active;
    const UINT64 v = ++sc->fenceValue;
    s->queue->Signal(sc->fence.Get(), v);
    if (sc->fence->GetCompletedValue() < v) {
        sc->fence->SetEventOnCompletion(v, sc->fenceEvent);
        WaitForSingleObjectEx(sc->fenceEvent, INFINITE, FALSE);
    }
}

bool makeTargets(D3d12Instance* s, D3d12Swapchain* sc) {
    D3D12_CPU_DESCRIPTOR_HANDLE h = sc->rtvHeap->GetCPUDescriptorHandleForHeapStart();
    for (UINT n = 0; n < kFrameCount; ++n) {
        if (FAILED(sc->swap->GetBuffer(n, IID_PPV_ARGS(&sc->renderTargets[n])))) return false;
        s->device->CreateRenderTargetView(sc->renderTargets[n].Get(), nullptr, h);
        h.ptr += sc->rtvDescSize;
    }
    sc->frameIndex = sc->swap->GetCurrentBackBufferIndex();

    // Depth buffer (D32_FLOAT) + a 1-entry DSV heap.
    if (!sc->dsvHeap) {
        D3D12_DESCRIPTOR_HEAP_DESC dhd = {};
        dhd.NumDescriptors = 1; dhd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
        if (FAILED(s->device->CreateDescriptorHeap(&dhd, IID_PPV_ARGS(&sc->dsvHeap)))) return false;
    }
    sc->depth.Reset();
    D3D12_HEAP_PROPERTIES hp = {}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC dd = {};
    dd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    dd.Width = (UINT)sc->w; dd.Height = (UINT)sc->h; dd.DepthOrArraySize = 1; dd.MipLevels = 1;
    dd.Format = DXGI_FORMAT_D32_FLOAT; dd.SampleDesc.Count = 1;
    dd.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    D3D12_CLEAR_VALUE cv = {}; cv.Format = DXGI_FORMAT_D32_FLOAT; cv.DepthStencil.Depth = 1.0f;
    if (FAILED(s->device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &dd,
            D3D12_RESOURCE_STATE_DEPTH_WRITE, &cv, IID_PPV_ARGS(&sc->depth)))) return false;
    D3D12_DEPTH_STENCIL_VIEW_DESC dsv = {};
    dsv.Format = DXGI_FORMAT_D32_FLOAT; dsv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    s->device->CreateDepthStencilView(sc->depth.Get(), &dsv, sc->dsvHeap->GetCPUDescriptorHandleForHeapStart());
    return true;
}

RhiSwapchain* d3d12_swapchainCreate(RhiInstance* r, void* windowHandle, int w, int h, bool vsync) {
    D3d12Instance* s = self(r);
    D3d12Swapchain* sc = new D3d12Swapchain();
    sc->w = w; sc->h = h; sc->vsync = vsync;

    DXGI_SWAP_CHAIN_DESC1 desc = {};
    desc.BufferCount = kFrameCount;
    desc.Width       = (UINT)w;
    desc.Height      = (UINT)h;
    desc.Format      = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.SwapEffect  = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    desc.SampleDesc.Count = 1;
    desc.AlphaMode   = DXGI_ALPHA_MODE_IGNORE;

    ComPtr<IDXGISwapChain1> swap1;
    if (FAILED(s->factory->CreateSwapChainForHwnd(
            s->queue.Get(), (HWND)windowHandle, &desc, nullptr, nullptr, &swap1))) {
        delete sc; return nullptr;
    }
    s->factory->MakeWindowAssociation((HWND)windowHandle, DXGI_MWA_NO_ALT_ENTER);
    if (FAILED(swap1.As(&sc->swap))) { delete sc; return nullptr; }

    D3D12_DESCRIPTOR_HEAP_DESC hd = {};
    hd.NumDescriptors = kFrameCount;
    hd.Type  = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    if (FAILED(s->device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&sc->rtvHeap)))) {
        delete sc; return nullptr;
    }
    sc->rtvDescSize = s->device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    if (!makeTargets(s, sc)) { delete sc; return nullptr; }

    for (UINT n = 0; n < kFrameCount; ++n) {
        if (FAILED(s->device->CreateCommandAllocator(
                D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&sc->allocators[n])))) {
            delete sc; return nullptr;
        }
    }

    if (!s->cmdList) {
        if (FAILED(s->device->CreateCommandList(
                0, D3D12_COMMAND_LIST_TYPE_DIRECT, sc->allocators[0].Get(),
                nullptr, IID_PPV_ARGS(&s->cmdList)))) {
            delete sc; return nullptr;
        }
        s->cmdList->Close();
    }

    if (FAILED(s->device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&sc->fence)))) {
        delete sc; return nullptr;
    }
    sc->fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!sc->fenceEvent) { delete sc; return nullptr; }

    s->active = sc;
    return reinterpret_cast<RhiSwapchain*>(sc);
}

void d3d12_swapchainDestroy(RhiInstance* r, RhiSwapchain* h) {
    D3d12Instance* s = self(r);
    D3d12Swapchain* sc = reinterpret_cast<D3d12Swapchain*>(h);
    if (!sc) return;
    if (s->active == sc) { waitIdle(s); s->active = nullptr; }
    if (sc->fenceEvent) CloseHandle(sc->fenceEvent);
    delete sc;
}

void d3d12_swapchainResize(RhiInstance* r, RhiSwapchain* h, int w, int t) {
    D3d12Instance* s = self(r);
    D3d12Swapchain* sc = reinterpret_cast<D3d12Swapchain*>(h);
    if (!sc || !sc->swap) return;
    waitIdle(s);
    for (UINT n = 0; n < kFrameCount; ++n) sc->renderTargets[n].Reset();
    sc->swap->ResizeBuffers(kFrameCount, (UINT)w, (UINT)t, DXGI_FORMAT_UNKNOWN, 0);
    sc->w = w; sc->h = t;
    makeTargets(s, sc);
}

// (Re)create the persistently-mapped vertex-upload ring at `cap` bytes. Only called from
// beginFrame, where present()'s waitIdle guarantees the GPU no longer reads the old buffer.
static bool ensureUploadCap(D3d12Instance* s, UINT cap) {
    if (cap <= s->uploadCap && s->uploadVB) return true;
    if (s->uploadVB) { s->uploadVB->Unmap(0, nullptr); s->uploadVB.Reset(); s->mappedVB = nullptr; }
    D3D12_HEAP_PROPERTIES hp = {}; hp.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC rd = {};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; rd.Width = cap; rd.Height = 1;
    rd.DepthOrArraySize = 1; rd.MipLevels = 1; rd.Format = DXGI_FORMAT_UNKNOWN;
    rd.SampleDesc.Count = 1; rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (FAILED(s->device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&s->uploadVB)))) return false;
    D3D12_RANGE none = {0, 0};
    if (FAILED(s->uploadVB->Map(0, &none, &s->mappedVB))) { s->uploadVB.Reset(); return false; }
    s->uploadCap = cap;
    return true;
}

// Sub-allocate `bytes` from the frame's ring (16-byte aligned). Returns false if the frame
// overflowed the ring; records the needed size so beginFrame can grow it next frame.
static bool ringAlloc(D3d12Instance* s, UINT bytes, D3D12_GPU_VIRTUAL_ADDRESS* gpu, void** cpu) {
    UINT off = (s->uploadOffset + 15u) & ~15u;
    if (!s->uploadVB || off + bytes > s->uploadCap) {
        UINT want = off + bytes; if (want > s->uploadWant) s->uploadWant = want;
        return false;
    }
    *gpu = s->uploadVB->GetGPUVirtualAddress() + off;
    *cpu = (char*)s->mappedVB + off;
    s->uploadOffset = off + bytes;
    return true;
}

void d3d12_beginFrame(RhiInstance* r) {
    D3d12Instance* s = self(r);
    D3d12Swapchain* sc = s->active;
    if (!sc) return;
    // Grow the vertex-upload ring to last frame's high-water mark (GPU idle here), then reset.
    UINT want = s->uploadWant > (1u << 20) ? s->uploadWant : (1u << 20);
    if (want > s->uploadCap) { UINT cap = s->uploadCap ? s->uploadCap : (1u << 20);
                               while (cap < want) cap <<= 1; ensureUploadCap(s, cap); }
    s->uploadOffset = 0;
    ID3D12CommandAllocator* alloc = sc->allocators[sc->frameIndex].Get();
    alloc->Reset();
    s->cmdList->Reset(alloc, nullptr);
    barrier(s->cmdList.Get(), sc->renderTargets[sc->frameIndex].Get(),
            D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtvHandle(sc);
    D3D12_CPU_DESCRIPTOR_HANDLE dsv = sc->dsvHeap->GetCPUDescriptorHandleForHeapStart();
    s->cmdList->OMSetRenderTargets(1, &rtv, FALSE, &dsv);
    s->cmdList->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);

    D3D12_VIEWPORT vp = {}; vp.Width = (FLOAT)sc->w; vp.Height = (FLOAT)sc->h; vp.MaxDepth = 1.0f;
    D3D12_RECT     rc = {}; rc.right = sc->w; rc.bottom = sc->h;
    s->cmdList->RSSetViewports(1, &vp);
    s->cmdList->RSSetScissorRects(1, &rc);
}

void d3d12_clear(RhiInstance* r, float cr, float cg, float cb, float ca) {
    D3d12Instance* s = self(r);
    D3d12Swapchain* sc = s->active;
    if (!sc) return;
    const float color[4] = { cr, cg, cb, ca };
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtvHandle(sc);
    s->cmdList->ClearRenderTargetView(rtv, color, 0, nullptr);
}

void d3d12_endFrame(RhiInstance* r) {
    D3d12Instance* s = self(r);
    D3d12Swapchain* sc = s->active;
    if (!sc) return;
    barrier(s->cmdList.Get(), sc->renderTargets[sc->frameIndex].Get(),
            D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
    s->cmdList->Close();
    ID3D12CommandList* lists[] = { s->cmdList.Get() };
    s->queue->ExecuteCommandLists(1, lists);
}

bool d3d12_present(RhiInstance* r, RhiSwapchain* h) {
    D3d12Instance* s = self(r);
    D3d12Swapchain* sc = reinterpret_cast<D3d12Swapchain*>(h);
    if (!sc || !sc->swap) return false;
    HRESULT hr = sc->swap->Present(sc->vsync ? 1 : 0, 0);
    if (FAILED(hr)) return false;
    waitIdle(s); // flush; simple + correct for bring-up
    sc->frameIndex = sc->swap->GetCurrentBackBufferIndex();
    return true;
}

static const char* kColorHLSL12 =
    "cbuffer Xform : register(b0) { float4 uRows[4]; };\n"
    "struct VSIn  { float3 pos : POSITION; float4 col : COLOR; };\n"
    "struct VSOut { float4 pos : SV_Position; float4 col : COLOR; };\n"
    "VSOut vsmain(VSIn i){ float4 p = float4(i.pos, 1.0); VSOut o;\n"
    "  o.pos = float4(dot(uRows[0],p), dot(uRows[1],p), dot(uRows[2],p), dot(uRows[3],p));\n"
    "  o.col = i.col; return o; }\n"
    "float4 psmain(VSOut i) : SV_Target { return i.col; }\n";

bool buildPipeline12(D3d12Instance* s) {
    if (s->pipelineReady) return true;

    D3D12_ROOT_PARAMETER rp = {};
    rp.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    rp.Constants.ShaderRegister = 0;   // b0
    rp.Constants.Num32BitValues = 16;  // 4x4 MVP
    rp.ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;

    D3D12_ROOT_SIGNATURE_DESC rsd = {};
    rsd.NumParameters = 1;
    rsd.pParameters = &rp;
    rsd.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    ComPtr<ID3DBlob> sig, serr;
    if (FAILED(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &serr))) return false;
    if (FAILED(s->device->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(),
                                              IID_PPV_ARGS(&s->rootSig)))) return false;

    ComPtr<ID3DBlob> vsb, psb, cerr;
    if (FAILED(D3DCompile(kColorHLSL12, strlen(kColorHLSL12), "color", nullptr, nullptr,
                          "vsmain", "vs_5_0", 0, 0, &vsb, &cerr))) return false;
    if (FAILED(D3DCompile(kColorHLSL12, strlen(kColorHLSL12), "color", nullptr, nullptr,
                          "psmain", "ps_5_0", 0, 0, &psb, &cerr))) return false;

    D3D12_INPUT_ELEMENT_DESC elems[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,  D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "COLOR",    0, DXGI_FORMAT_R8G8B8A8_UNORM,  0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    };

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd = {};
    pd.pRootSignature = s->rootSig.Get();
    pd.VS = { vsb->GetBufferPointer(), vsb->GetBufferSize() };
    pd.PS = { psb->GetBufferPointer(), psb->GetBufferSize() };
    pd.InputLayout = { elems, 2 };
    pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pd.RasterizerState.DepthClipEnable = TRUE;
    pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pd.DepthStencilState.DepthEnable = TRUE;
    pd.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    pd.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
    pd.DepthStencilState.StencilEnable = FALSE;
    pd.SampleMask = UINT_MAX;
    pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pd.NumRenderTargets = 1;
    pd.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    pd.DSVFormat = DXGI_FORMAT_D32_FLOAT;
    pd.SampleDesc.Count = 1;
    if (FAILED(s->device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&s->pso)))) return false;

    s->pipelineReady = true;
    return true;
}

void d3d12_drawColored(RhiInstance* r, const RhiColorVertex* verts, uint32_t count) {
    D3d12Instance* s = self(r);
    if (!verts || count == 0 || !s->active) return;
    if (!buildPipeline12(s)) return;

    const UINT stride = (UINT)sizeof(RhiColorVertex);
    const UINT needed = stride * count;
    D3D12_GPU_VIRTUAL_ADDRESS gpu; void* cpu;
    if (!ringAlloc(s, needed, &gpu, &cpu)) return;   // frame overflow: ring grows next frame
    memcpy(cpu, verts, needed);

    D3D12_VERTEX_BUFFER_VIEW vbv = {};
    vbv.BufferLocation = gpu;
    vbv.SizeInBytes = needed;
    vbv.StrideInBytes = stride;

    s->cmdList->SetGraphicsRootSignature(s->rootSig.Get());
    s->cmdList->SetGraphicsRoot32BitConstants(0, 16, s->mvp, 0);
    s->cmdList->SetPipelineState(s->pso.Get());
    s->cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    s->cmdList->IASetVertexBuffers(0, 1, &vbv);
    s->cmdList->DrawInstanced(count, 1, 0, 0);
}

void d3d12_setColorTransform(RhiInstance* r, const float m[16]) {
    memcpy(self(r)->mvp, m, 16 * sizeof(float));
}

static const char* kTexHLSL12 =
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
    "  clip(c.a - uAlpha.x);\n"
    "  return c; }\n";

bool buildTexPipeline12(D3d12Instance* s) {
    if (s->texPipelineReady) return true;

    D3D12_DESCRIPTOR_HEAP_DESC hd = {};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.NumDescriptors = 4096;   // one SRV per loaded texture (terrain + object models)
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(s->device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&s->srvHeap)))) return false;
    s->srvDescSize = s->device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    D3D12_DESCRIPTOR_RANGE srvRange = {};
    srvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    srvRange.NumDescriptors = 1;
    srvRange.BaseShaderRegister = 0; // t0

    D3D12_ROOT_PARAMETER params[3] = {};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[0].Constants.ShaderRegister = 0; // b0
    params[0].Constants.Num32BitValues = 16;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable.NumDescriptorRanges = 1;
    params[1].DescriptorTable.pDescriptorRanges = &srvRange;
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[2].Constants.ShaderRegister = 1; // b1 (alpha-test ref)
    params[2].Constants.Num32BitValues = 4;
    params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_STATIC_SAMPLER_DESC samp = {};
    samp.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    samp.AddressU = samp.AddressV = samp.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    samp.MaxLOD = D3D12_FLOAT32_MAX;
    samp.ShaderRegister = 0; // s0
    samp.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC rsd = {};
    rsd.NumParameters = 3;
    rsd.pParameters = params;
    rsd.NumStaticSamplers = 1;
    rsd.pStaticSamplers = &samp;
    rsd.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    ComPtr<ID3DBlob> sig, serr;
    if (FAILED(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &serr))) return false;
    if (FAILED(s->device->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(), IID_PPV_ARGS(&s->rootSigTex)))) return false;

    ComPtr<ID3DBlob> vsb, psb, cerr;
    if (FAILED(D3DCompile(kTexHLSL12, strlen(kTexHLSL12), "tex", nullptr, nullptr, "vsmain", "vs_5_0", 0, 0, &vsb, &cerr))) return false;
    if (FAILED(D3DCompile(kTexHLSL12, strlen(kTexHLSL12), "tex", nullptr, nullptr, "psmain", "ps_5_0", 0, 0, &psb, &cerr))) return false;

    D3D12_INPUT_ELEMENT_DESC elems[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,  D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "COLOR",    0, DXGI_FORMAT_R8G8B8A8_UNORM,  0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,    0, 16, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    };
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd = {};
    pd.pRootSignature = s->rootSigTex.Get();
    pd.VS = { vsb->GetBufferPointer(), vsb->GetBufferSize() };
    pd.PS = { psb->GetBufferPointer(), psb->GetBufferSize() };
    pd.InputLayout = { elems, 3 };
    pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pd.RasterizerState.DepthClipEnable = TRUE;
    pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pd.DepthStencilState.DepthEnable = TRUE;
    pd.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    pd.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
    pd.SampleMask = UINT_MAX;
    pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pd.NumRenderTargets = 1;
    pd.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    pd.DSVFormat = DXGI_FORMAT_D32_FLOAT;
    pd.SampleDesc.Count = 1;
    if (FAILED(s->device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&s->psoTex)))) return false;

    // Blend variant: src-over alpha blend, depth test on but writes off (translucent geometry).
    pd.BlendState.RenderTarget[0].BlendEnable    = TRUE;
    pd.BlendState.RenderTarget[0].SrcBlend       = D3D12_BLEND_SRC_ALPHA;
    pd.BlendState.RenderTarget[0].DestBlend      = D3D12_BLEND_INV_SRC_ALPHA;
    pd.BlendState.RenderTarget[0].BlendOp        = D3D12_BLEND_OP_ADD;
    pd.BlendState.RenderTarget[0].SrcBlendAlpha  = D3D12_BLEND_ONE;
    pd.BlendState.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
    pd.BlendState.RenderTarget[0].BlendOpAlpha   = D3D12_BLEND_OP_ADD;
    pd.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    if (FAILED(s->device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&s->psoTexBlend)))) return false;

    s->texPipelineReady = true;
    return true;
}

RhiTexture* d3d12_createTexture(RhiInstance* r, int w, int h, int mips, uint32_t fmt, const void* rgba8) {
    (void)mips; (void)fmt;
    D3d12Instance* s = self(r);
    if (w <= 0 || h <= 0 || !rgba8) return nullptr;
    if (!buildTexPipeline12(s)) return nullptr;

    // Default-heap texture in COPY_DEST.
    D3D12_HEAP_PROPERTIES defHeap = {}; defHeap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC td = {};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = (UINT)w; td.Height = (UINT)h; td.DepthOrArraySize = 1; td.MipLevels = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    D3d12Texture* t = new D3d12Texture();
    if (FAILED(s->device->CreateCommittedResource(&defHeap, D3D12_HEAP_FLAG_NONE, &td,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&t->res)))) { delete t; return nullptr; }

    // Upload buffer sized to the copyable footprint.
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp = {};
    UINT numRows = 0; UINT64 rowBytes = 0, total = 0;
    s->device->GetCopyableFootprints(&td, 0, 1, 0, &fp, &numRows, &rowBytes, &total);

    D3D12_HEAP_PROPERTIES upHeap = {}; upHeap.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC bd = {};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; bd.Width = total; bd.Height = 1;
    bd.DepthOrArraySize = 1; bd.MipLevels = 1; bd.Format = DXGI_FORMAT_UNKNOWN;
    bd.SampleDesc.Count = 1; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> upload;
    if (FAILED(s->device->CreateCommittedResource(&upHeap, D3D12_HEAP_FLAG_NONE, &bd,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&upload)))) { delete t; return nullptr; }

    uint8_t* map = nullptr;
    D3D12_RANGE none = {0, 0};
    upload->Map(0, &none, (void**)&map);
    const uint8_t* src = (const uint8_t*)rgba8;
    for (UINT y = 0; y < numRows; ++y)
        memcpy(map + fp.Offset + (SIZE_T)y * fp.Footprint.RowPitch, src + (SIZE_T)y * w * 4, (SIZE_T)w * 4);
    upload->Unmap(0, nullptr);

    // One-shot copy on a temporary allocator/list + fence.
    ComPtr<ID3D12CommandAllocator> alloc;
    ComPtr<ID3D12GraphicsCommandList> list;
    s->device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc));
    s->device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(), nullptr, IID_PPV_ARGS(&list));

    D3D12_TEXTURE_COPY_LOCATION dstLoc = {}; dstLoc.pResource = t->res.Get();
    dstLoc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; dstLoc.SubresourceIndex = 0;
    D3D12_TEXTURE_COPY_LOCATION srcLoc = {}; srcLoc.pResource = upload.Get();
    srcLoc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; srcLoc.PlacedFootprint = fp;
    list->CopyTextureRegion(&dstLoc, 0, 0, 0, &srcLoc, nullptr);

    D3D12_RESOURCE_BARRIER b = {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = t->res.Get();
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    list->ResourceBarrier(1, &b);
    list->Close();

    ID3D12CommandList* lists[] = { list.Get() };
    s->queue->ExecuteCommandLists(1, lists);
    ComPtr<ID3D12Fence> fence;
    s->device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
    HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    s->queue->Signal(fence.Get(), 1);
    if (fence->GetCompletedValue() < 1) { fence->SetEventOnCompletion(1, ev); WaitForSingleObjectEx(ev, INFINITE, FALSE); }
    CloseHandle(ev);

    // SRV in the shader-visible heap.
    UINT slot = s->srvNext++;
    D3D12_CPU_DESCRIPTOR_HANDLE cpu = s->srvHeap->GetCPUDescriptorHandleForHeapStart();
    cpu.ptr += (SIZE_T)slot * s->srvDescSize;
    D3D12_SHADER_RESOURCE_VIEW_DESC sd = {};
    sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sd.Texture2D.MipLevels = 1;
    s->device->CreateShaderResourceView(t->res.Get(), &sd, cpu);
    t->gpu = s->srvHeap->GetGPUDescriptorHandleForHeapStart();
    t->gpu.ptr += (UINT64)slot * s->srvDescSize;
    return reinterpret_cast<RhiTexture*>(t);
}

void d3d12_destroyTexture(RhiInstance* r, RhiTexture* h) { (void)r; delete reinterpret_cast<D3d12Texture*>(h); }

void d3d12_setTexture(RhiInstance* r, int slot, RhiTexture* h) {
    (void)slot;
    D3d12Instance* s = self(r);
    D3d12Texture* t = reinterpret_cast<D3d12Texture*>(h);
    if (t) { s->curTexGpu = t->gpu; s->curTexValid = true; } else s->curTexValid = false;
}

void d3d12_drawTextured(RhiInstance* r, const RhiTexVertex* verts, uint32_t count) {
    D3d12Instance* s = self(r);
    if (!verts || count == 0 || !s->active || !s->curTexValid) return;
    if (!buildTexPipeline12(s)) return;

    const UINT stride = (UINT)sizeof(RhiTexVertex);
    const UINT needed = stride * count;
    D3D12_GPU_VIRTUAL_ADDRESS gpu; void* cpu;
    if (!ringAlloc(s, needed, &gpu, &cpu)) return;   // frame overflow: ring grows next frame
    memcpy(cpu, verts, needed);

    D3D12_VERTEX_BUFFER_VIEW vbv = {};
    vbv.BufferLocation = gpu;
    vbv.SizeInBytes = needed; vbv.StrideInBytes = stride;

    int mode = r->alphaMode;
    float alpha4[4] = { (mode == RHI_ALPHA_TEST) ? 0.5f : -1.0f, 0, 0, 0 };

    ID3D12DescriptorHeap* heaps[] = { s->srvHeap.Get() };
    s->cmdList->SetDescriptorHeaps(1, heaps);
    s->cmdList->SetGraphicsRootSignature(s->rootSigTex.Get());
    s->cmdList->SetGraphicsRoot32BitConstants(0, 16, s->mvp, 0);
    s->cmdList->SetGraphicsRootDescriptorTable(1, s->curTexGpu);
    s->cmdList->SetGraphicsRoot32BitConstants(2, 4, alpha4, 0);
    s->cmdList->SetPipelineState(mode == RHI_ALPHA_BLEND ? s->psoTexBlend.Get() : s->psoTex.Get());
    s->cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    s->cmdList->IASetVertexBuffers(0, 1, &vbv);
    s->cmdList->DrawInstanced(count, 1, 0, 0);
}

void d3d12_destroy(RhiInstance* r) {
    D3d12Instance* s = self(r);
    if (!s) return;
    if (s->active) {
        waitIdle(s);
        if (s->active->fenceEvent) CloseHandle(s->active->fenceEvent);
        delete s->active;
        s->active = nullptr;
    }
    delete s;
}

const RhiOps kOps = {
    d3d12_destroy,
    d3d12_swapchainCreate,
    d3d12_swapchainDestroy,
    d3d12_swapchainResize,
    d3d12_present,
    d3d12_beginFrame,
    d3d12_endFrame,
    d3d12_clear,
    d3d12_drawColored,
    d3d12_setColorTransform,
    d3d12_createTexture,
    d3d12_destroyTexture,
    d3d12_setTexture,
    d3d12_drawTextured,
};

bool pickAdapter(IDXGIFactory4* factory, ComPtr<IDXGIAdapter1>& out) {
    ComPtr<IDXGIAdapter1> adapter;
    for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
        DXGI_ADAPTER_DESC1 d; adapter->GetDesc1(&d);
        if (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;
        if (SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0,
                                        __uuidof(ID3D12Device), nullptr))) {
            out = adapter;
            return true;
        }
    }
    return false;
}

} // namespace

extern "C" bool rhi_d3d12_available(void) {
    ComPtr<IDXGIFactory4> factory;
    if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)))) return false;
    ComPtr<IDXGIAdapter1> adapter;
    return pickAdapter(factory.Get(), adapter);
}

extern "C" RhiInstance* rhi_d3d12_create(const RhiCreateInfo* info) {
    UINT factoryFlags = 0;
    if (info->debug) {
        ComPtr<ID3D12Debug> dbg;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&dbg)))) {
            dbg->EnableDebugLayer();
            factoryFlags |= DXGI_CREATE_FACTORY_DEBUG;
        }
    }

    D3d12Instance* s = new D3d12Instance();
    s->base.ops     = &kOps;
    s->base.backend = RHI_BACKEND_D3D12;

    if (FAILED(CreateDXGIFactory2(factoryFlags, IID_PPV_ARGS(&s->factory)))) { delete s; return nullptr; }

    ComPtr<IDXGIAdapter1> adapter;
    if (!pickAdapter(s->factory.Get(), adapter)) { delete s; return nullptr; }
    if (FAILED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&s->device)))) {
        delete s; return nullptr;
    }

    D3D12_COMMAND_QUEUE_DESC qd = {};
    qd.Type  = D3D12_COMMAND_LIST_TYPE_DIRECT;
    qd.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    if (FAILED(s->device->CreateCommandQueue(&qd, IID_PPV_ARGS(&s->queue)))) { delete s; return nullptr; }

    (void)info;
    return &s->base;
}
