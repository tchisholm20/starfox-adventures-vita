#if defined(_WIN32)   /* vita fork: Windows-only (VEH MMIO trap). The Vita has no equivalent yet: C TUs
                         that write GXWGFifo directly draw NOTHING there (docs/47). */
// wgpipe_trap.c - MMIO trap for the GX write-gather pipe (WGPIPE) at 0xCC008000.
//
// THE PROBLEM. On the GameCube the write-gather pipe is a single MMIO register at
// 0xCC008000; each CPU store to it appends bytes to the GP FIFO (a hardware side
// effect - successive writes accumulate, they do not overwrite). Game TUs submit
// immediate-mode 2D/text geometry by storing straight into it (`GXWGFifo.s16 = x`,
// via GXVert.h's AT_ADDRESS(0xCC008000)). The port routes these into a software FIFO
// interpreter (gx_draw.c) two ways: C++ TUs get an operator= write-proxy
// (ppcwgpipe_struct.h); but a C TU that is not C++-clean (e.g. src/track/
// intersect_render.c, which has void*->T*/int->enum C-isms C++ rejects, and which we
// compile unmodified) cannot use that proxy - as C its stores hit the raw address and
// fault.
//
// THE FIX (this file). Leave 0xCC008000 unmapped so every store to it faults, and
// install a vectored exception handler that, on a write-fault to that address, decodes
// the (small, fixed) set of MOV encodings MSVC emits for a store to that constant
// direct address, feeds the value into the SAME gxfifo_push_* path the C++ proxy uses
// (big-endian, into gx_draw's FIFO), advances EIP past the instruction, and resumes.
// This is the same mechanism a GC emulator (Dolphin) uses for the WGPIPE, adapted to a
// native exception handler; it works for ANY C TU that writes the FIFO directly.
//
// COST: one page fault per FIFO store (~hundreds per 2D-heavy frame). Acceptable for
// bring-up; optimize later if a scene makes it hot. Correctness first.

#include <windows.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>

// The software-FIFO sinks (gx_draw.c), same ones the C++ write-proxy calls.
extern void gxfifo_push_u8(unsigned char v);
extern void gxfifo_push_u16(unsigned short v);
extern void gxfifo_push_u32(unsigned int v);

#define WGPIPE_ADDR 0xCC008000u

// x86 GPR by ModRM reg index (0=eax..7=edi).
static uint32_t gpr32(const CONTEXT* c, int idx) {
    switch (idx) {
        case 0: return c->Eax; case 1: return c->Ecx; case 2: return c->Edx; case 3: return c->Ebx;
        case 4: return c->Esp; case 5: return c->Ebp; case 6: return c->Esi; case 7: return c->Edi;
    }
    return 0;
}
// 8-bit GPR (0=al 1=cl 2=dl 3=bl 4=ah 5=ch 6=dh 7=bh).
static uint8_t gpr8(const CONTEXT* c, int idx) {
    uint32_t base = gpr32(c, idx & 3);
    return (idx < 4) ? (uint8_t)base : (uint8_t)(base >> 8);
}
// Low dword of xmm[idx] from the FXSAVE area (XMM0 @ offset 0xA0, 16 bytes each).
static uint32_t xmm_lo(const CONTEXT* c, int idx) {
    uint32_t v;
    memcpy(&v, &c->ExtendedRegisters[0xA0 + idx * 16], 4);
    return v;
}

static LONG CALLBACK wgpipeVeh(EXCEPTION_POINTERS* ep) {
    EXCEPTION_RECORD* er = ep->ExceptionRecord;
    if (er->ExceptionCode != EXCEPTION_ACCESS_VIOLATION) return EXCEPTION_CONTINUE_SEARCH;
    if (er->ExceptionInformation[0] != 1) return EXCEPTION_CONTINUE_SEARCH;              // not a write
    if (er->ExceptionInformation[1] != WGPIPE_ADDR) return EXCEPTION_CONTINUE_SEARCH;    // not the FIFO port

    CONTEXT* c = ep->ContextRecord;
    const uint8_t* p = (const uint8_t*)c->Eip;
    int len;

    // Every store targets the constant direct address 0xCC008000 (already confirmed by
    // the fault address), so we classify size + value-source + length only.
    if (p[0] == 0xF3 && p[1] == 0x0F && p[2] == 0x11) {            // movss [addr], xmm   (f32; bits == u32)
        gxfifo_push_u32(xmm_lo(c, (p[3] >> 3) & 7));  len = 8;
    } else if (p[0] == 0x66) {                                     // 16-bit operand
        if (p[1] == 0xA3)      { gxfifo_push_u16((uint16_t)gpr32(c, 0)); len = 6; }              // mov [moffs], ax
        else if (p[1] == 0x89) { gxfifo_push_u16((uint16_t)gpr32(c, (p[2] >> 3) & 7)); len = 7; } // mov [addr], r16
        else if (p[1] == 0xC7) { uint16_t imm; memcpy(&imm, p + 7, 2); gxfifo_push_u16(imm); len = 9; } // mov [addr], imm16
        else return EXCEPTION_CONTINUE_SEARCH;
    } else if (p[0] == 0xC6 && p[1] == 0x05) { gxfifo_push_u8(p[6]); len = 7; }          // mov byte [addr], imm8
    else if (p[0] == 0xA2)                   { gxfifo_push_u8((uint8_t)gpr32(c, 0)); len = 5; }  // mov [moffs], al
    else if (p[0] == 0x88)                   { gxfifo_push_u8(gpr8(c, (p[1] >> 3) & 7)); len = 6; } // mov byte [addr], r8
    else if (p[0] == 0xC7 && p[1] == 0x05)   { uint32_t imm; memcpy(&imm, p + 6, 4); gxfifo_push_u32(imm); len = 10; } // mov dword [addr], imm32
    else if (p[0] == 0xA3)                   { gxfifo_push_u32(gpr32(c, 0)); len = 5; }  // mov [moffs], eax
    else if (p[0] == 0x89)                   { gxfifo_push_u32(gpr32(c, (p[1] >> 3) & 7)); len = 6; } // mov dword [addr], r32
    else {
        // Unhandled encoding: report it and let it crash rather than silently corrupt
        // the FIFO. Add the form here when this fires.
        char msg[160];
        _snprintf(msg, sizeof msg,
                  "[wgpipe] unhandled store encoding at %p: %02X %02X %02X %02X %02X\n",
                  (void*)c->Eip, p[0], p[1], p[2], p[3], p[4]);
        OutputDebugStringA(msg);
        fputs(msg, stderr);
        return EXCEPTION_CONTINUE_SEARCH;
    }

    c->Eip += len;
    return EXCEPTION_CONTINUE_EXECUTION;
}

// Install once, before any GX submission. Idempotent.
void stairfax_wgpipe_trap_install(void) {
    static int installed = 0;
    if (installed) return;
    installed = 1;
    AddVectoredExceptionHandler(1 /* call first */, wgpipeVeh);
}

// Auto-arm at CRT init, BEFORE main() - the game writes GXWGFifo during init()
// (font-atlas / early 2D), well before any port render hook runs. Registering
// first (priority 1) puts us ahead of the port's crash handler (added last,
// priority 0), so WGPIPE write-faults are consumed here and everything else still
// reaches the crash reporter. This TU is compiled straight into game_engine, so
// the .CRT$XCU entry is never stripped.
static int wgpipe_autoinstall(void) { stairfax_wgpipe_trap_install(); return 0; }
#pragma section(".CRT$XCU", read)
__declspec(allocate(".CRT$XCU")) static int (*wgpipe_autoinstall_ptr)(void) = wgpipe_autoinstall;
#endif
