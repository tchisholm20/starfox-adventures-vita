// objseq_endian.c - ANIMCURV.bin sequence data (dlls/engine/2/2.c objLoadAnimdata) to host order.
// 2026-09-28, Vita run 81: the ShipBattle prologue cutscene restarted 83 times in ~60 frames (its
// sequence header and commands were read big-endian), objects piled up past sceneDraw's 616-byte
// per-object opacity buffer, and the overflow smashed a saved register (r4 = 0x101 -> data abort).
//
// A record is an 8-byte header {char tag[4] "SEQA"/"SEQB"; s16 dataSize; s16 commandCount} and then
// dataSize bytes: commandCount 4-byte words, then 8-byte curve keys (ObjCurveKey {f32 value;
// s8 tangentAndMode; u8 track; s16 frame}). The words are of TWO kinds, as the interpreter reads them:
//   - a command, ObjSeqCommand {s8 opcode; u8 frameDelta; s16 param}: swap the s16;
//   - after a SEQACT_CONDITION (0x0B) command with param > 0, `param` sub-command words that
//     seqDoSubCmd0B / objSeqFindLabel / objSeqFindConditional read as a u32 (`packed & 0x3f`,
//     `packed >> 16`): reverse all four bytes. Every scan of the stream skips them the same way
//     (`commandIndex += command->param`), so the walk below is the interpreter's own.
// Both run on the COPIES objLoadAnimdata makes (a stack header, an mmAlloc'd body): the resident
// file itself stays big-endian, so there is nothing to double-swap.
#include <stdint.h>

static inline void sw16(uint8_t* p) { uint8_t t = p[0]; p[0] = p[1]; p[1] = t; }
static inline void sw32(uint8_t* p) { uint8_t t = p[0]; p[0] = p[3]; p[3] = t; t = p[1]; p[1] = p[2]; p[2] = t; }

void stfx_objseq_anim_header(void* hdr) {
    sw16((uint8_t*)hdr + 4);   // dataSize
    sw16((uint8_t*)hdr + 6);   // commandCount
}

// Returns the number of words the walk consumed beyond commandCount (0 = the structure closed
// exactly; a CONDITION whose sub-words run past the end is clipped there and counted).
int stfx_objseq_anim_body(void* body, int commandCount, int dataSize) {
    uint8_t* b = (uint8_t*)body;
    int i = 0, over = 0;
    if (commandCount < 0 || dataSize < 0 || commandCount * 4 > dataSize) return -1;
    while (i < commandCount) {
        uint8_t* c = b + i * 4;
        int16_t param;
        sw16(c + 2);
        param = (int16_t)(c[2] | c[3] << 8);
        i++;
#ifndef STFX_OBJSEQ_BREAK_SUBWORDS
        if ((int8_t)c[0] == 0x0B && param > 0) {
            int k;
            for (k = 0; k < param; k++, i++) {
                if (i >= commandCount) { over += param - k; break; }
                sw32(b + i * 4);
            }
        }
#endif
    }
    for (int o = commandCount * 4; o + 8 <= dataSize; o += 8) {
        sw32(b + o);       // value (f32)
        sw16(b + o + 6);   // frame
    }
    return over;
}

/* STFX cutscene-camera diagnostic (dlls/engine/2/2.c ObjSeq_updateCamera). One `[cam]` line per second of wall time while
   an object sequence owns the camera: sequence id, camera mode DLL id, the pose the sequence asked for (req = x y z pitch yaw
   roll; n/a when the camera was already running), the camera object's pose as it stood when the sequence ran (cam = x y z
   rotX rotY rotZ fov), and the distance between the two. In FIXED mode (0x4c) nothing corrects the pose, so a large
   `moved` means another writer (camcontrol blend / collision / a mode change) displaced it. */
#include <stdio.h>
#include <time.h>
#include <math.h>
void stfx_cam_diag(int seqId, int mode, int haveReq, const float* req, const float* cam) {
    static time_t last;
    time_t now = time(NULL);
    if (now == last) return;
    last = now;
    if (haveReq) {
        float dx = cam[0] - req[0], dy = cam[1] - req[1], dz = cam[2] - req[2];
        fprintf(stderr, "[cam] seq %d mode 0x%x req %.1f %.1f %.1f rot %.0f %.0f %.0f | cam %.1f %.1f %.1f rot %.0f %.0f %.0f fov %.0f | moved %.2f\n",
                seqId, mode, req[0], req[1], req[2], req[3], req[4], req[5], cam[0], cam[1], cam[2], cam[3], cam[4], cam[5], cam[6],
                sqrtf(dx * dx + dy * dy + dz * dz));
    } else {
        fprintf(stderr, "[cam] seq %d mode 0x%x req n/a | cam %.1f %.1f %.1f rot %.0f %.0f %.0f fov %.0f\n",
                seqId, mode, cam[0], cam[1], cam[2], cam[3], cam[4], cam[5], cam[6]);
    }
}

/* STFX seq-end diagnostic (dlls/engine/2/2.c endObjSequence). One `[seqend]` line per sequence slot ended: the slot, the
   sequence id the [seq] line shows, the slot's frame counter, then -- from the slot's seq object(s) -- how many were found,
   their curFrame / endFrame (as read from the ANIMCURV record), runState and whether it was a camera sequence, and WHY:
   "reason" names the branch inside 2.c; NULL means an OBJECT called ->endSequence() and `caller` (symbolise with the
   usual +offset tool) says which. endFrame is the SEQ "set max time" command's param (opcode 0xFF), so a frame at or
   past it with the END command as reason is a normal finish; a frame well short of it is an early kill. */
void stfx_seqend_diag(int slot, int seqId, float slotFrame, int found, int curFrame, int endFrame, int runState,
                      int isCam, const char* why, void* caller) {
    fprintf(stderr, "[seqend] slot %d id %d slot-frame %.0f | seq objects %d: curFrame %d endFrame %d runState %d camera %d | %s%s caller %p\n",
            slot, seqId, (double)slotFrame, found, curFrame, endFrame, runState, isCam,
            why ? "reason: " : "reason: external ->endSequence()", why ? why : "", caller);
}

/* STFX seq-start diagnostic: printf-style stderr line for dlls/engine/2/2.c ObjSeq_start ([seqstart]) and objects/275 ([seqobj]). */
#include <stdarg.h>
void stfx_seq_log(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}
