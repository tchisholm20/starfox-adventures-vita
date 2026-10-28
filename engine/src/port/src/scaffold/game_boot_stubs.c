// game_boot_stubs.c - host stand-ins for the last leaf calls videoInit()/
// waitNextFrame() make into other subsystems (PPC supervisor regs, the paired-
// single matrix lib, the flip ring-queue, camera/PE helpers, the retrace swap).
//
// Defined with local prototypes and no game headers on purpose: C links by bare
// symbol, so the game TU's real signatures bind to these. The matrix helpers do
// the real math (cheap, and keeps any state the game reads back sane); the rest
// are host no-ops - there is no PPC MSR/HID0, no GP FIFO ring, and the VI shim
// already presents each frame, so the game's own swap callback has nothing to do.

// --- PPC supervisor registers (no MSR/HID0 on the host) --------------------
unsigned PPCMfmsr(void)        { return 0; }
void     PPCMtmsr(unsigned v)  { (void)v; }
unsigned PPCMfhid0(void)       { return 0; }
void     PPCMthid0(unsigned v) { (void)v; }

// --- paired-single matrix lib (real math; Mtx = f32[3][4], Mtx44 = f32[4][4]) --
void PSMTXIdentity(void* mm) {
    float* m = (float*)mm;
    int i; for (i = 0; i < 12; ++i) m[i] = 0.0f;
    m[0] = m[5] = m[10] = 1.0f;
}
void C_MTXOrtho(void* mm, float t, float b, float l, float r, float n, float f) {
    float (*m)[4] = (float(*)[4])mm;
    float tmp;
    tmp = 1.0f / (r - l);
    m[0][0] = 2.0f * tmp; m[0][1] = 0.0f; m[0][2] = 0.0f; m[0][3] = -(r + l) * tmp;
    tmp = 1.0f / (t - b);
    m[1][0] = 0.0f; m[1][1] = 2.0f * tmp; m[1][2] = 0.0f; m[1][3] = -(t + b) * tmp;
    tmp = 1.0f / (f - n);
    m[2][0] = 0.0f; m[2][1] = 0.0f; m[2][2] = -tmp; m[2][3] = -f * tmp;
    m[3][0] = 0.0f; m[3][1] = 0.0f; m[3][2] = 0.0f; m[3][3] = 1.0f;
}

// Queue_GetCount/Push/Init are now real (queue.c via the object/camera subtree).

// --- camera / PE helpers (GP state; no-op on host) -------------------------
// Camera_ApplyFullViewport is now the real camera.c version.
// gxSetPeControl_ZCompLoc_/gxSetZMode_ now provided real by intersect_render.c in
// game_engine (text/2D render bring-up) - stubs removed. Not referenced by game_boot.

// --- retrace swap callback (VI shim already presents the frame) ------------
void videoSwapFrameBuffers(unsigned int retraceCount)   { (void)retraceCount; }
