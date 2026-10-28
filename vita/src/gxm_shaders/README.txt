Four precompiled GXP programs copied UNCHANGED from vitaGL
(tools/_download/vitaGL/source/shaders/precompiled_*.h, Rinnegatamante/vitaGL, LGPL-3.0).
The Cg source of each is the comment at the top of its header; src/main.c compiles the
SAME source at runtime through vitaShaRK in the default mode, so the two modes feed
the identical patcher/draw path and differ only in where the SceGxmProgram comes from.
They are used ONLY by `shaders.txt = precompiled` -- the diagnostic mode that lets
everything except runtime compilation be exercised where libshacccg.suprx is absent.
