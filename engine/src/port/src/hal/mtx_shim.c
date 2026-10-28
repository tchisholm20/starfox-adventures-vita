// mtx_shim.c - for host we keep dolphin/mtx/*.c as-is where possible.
// This stub exists so CMake has a target; real impl is either:
// - compile src/dolphin/mtx/*.c on host (they are ANSI C, no HW regs)
// - or swap to glm.
// For now noop; PSMTX is in src/dolphin/mtx/psmtx.c which compiles cleanly on x86.
