// game_objanim_stubs.c - the three siblings objanim.c needs that aren't ported yet.
//
// objanim.c (the real move-control TU: Object_ObjAnimSetMove + the ObjAnim_* move API) links
// into game_engine. Its ObjAnim_LoadCachedMove/ObjAnim_LoadMoveEvents deps are already real in
// model.c/object.c; only ObjHitReact_LoadMoveEntries (the hit-react move table, part of the
// object-DLL animation subsystem, not up yet) needs a no-op stub. Headerless like other *_stubs.c.

void ObjHitReact_LoadMoveEntries(void* a, void* b, void* c) { (void)a; (void)b; (void)c; }
