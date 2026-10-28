"""LLDB command for tools/model_skinning_fpr/run_trace.py: record GC/1.3 FPR save/restore emission."""
import lldb, json, struct
SAVE_ENTRY=0x4f72b0
REST_ENTRY=0x4f70e0
CALLS=[0x4f731c,0x4f7354,0x4f7375,0x4f73a9,0x4f73cc,0x4f73f2,0x4f743a,0x4f744c,0x4f745d,
       0x4f7155,0x4f71b4,0x4f71de,0x4f7230,0x4f727a,0x4f728c,0x4f729d]
def rd(p,a,n):
    e=lldb.SBError(); b=p.ReadMemory(a,n,e); return b if e.Success() else None
def w(p,a): return struct.unpack('<I',rd(p,a,4))[0]
def fname(p):
    obj=w(p,0x5e6610)
    if not obj: return None
    s=rd(p,w(p,obj+0xa)+0xa,96); return s.split(b'\0')[0].decode('latin1')
def run(debugger, command, result, internal_dict):
    out=command.strip()
    t=debugger.GetSelectedTarget(); p=t.GetProcess()
    for a in [SAVE_ENTRY,REST_ENTRY]+CALLS: t.BreakpointCreateByAddress(a)
    ev=[]
    while True:
        p.Continue()
        if p.GetState()!=lldb.eStateStopped: break
        fr=p.GetSelectedThread().GetFrameAtIndex(0); pc=fr.GetPC()
        sp=fr.FindRegister('esp').GetValueAsUnsigned()
        N=w(p,0x5e6758)
        if pc in (SAVE_ENTRY,REST_ENTRY):
            ev.append(dict(kind='save_entry' if pc==SAVE_ENTRY else 'restore_entry',function=fname(p),fpr_count=N))
        else:
            args=list(struct.unpack('<6I',rd(p,sp,24)))
            ev.append(dict(kind='emit',site=hex(pc),function=fname(p),fpr_count=N,opcode=hex(args[0]),args=args))
    json.dump(ev,open(out,'w'),indent=1)
    result.AppendMessage('events %d'%len(ev))
