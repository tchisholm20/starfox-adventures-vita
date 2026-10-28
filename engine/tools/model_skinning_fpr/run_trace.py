#!/usr/bin/env python3
"""Trace GC/1.3's prologue FPR-save emitter while it compiles the skinning C reference.

Compiles docs/foreign/model_skinning_reference.c with main/model.c's exact flags,
inside the ../mwcc offline Docker/QEMU sandbox, with host LLDB attached over
gdb-remote. Breakpoints sit on the FPR save/restore emitters (0x4f72b0/0x4f70e0)
and on every instruction-constructor call inside them; each hit records the
saved-FPR count at 0x5e6758 and the opcode/register the compiler pushes. The
traced object must be byte-identical to an untraced compile.
See docs/foreign/model_skinning_provenance.md.
"""
import argparse, collections, json, os, shlex, socket, subprocess, threading
from pathlib import Path

SFA = Path(__file__).resolve().parents[2]
IMAGE = "sha256:c1db70b793f51fe71a66604fccc75d0c1eae7628d3b2a1856e2798c0bdc2085c"
COMPILER_SHA = "4e502c38465500d4fda8d966b268151a6c74c730508e3d9b7efd23d1a6083715"
NAME = "mwcc-skinning-fpr-probe"

ap = argparse.ArgumentParser(description=__doc__)
ap.add_argument("--mwcc", type=Path, default=SFA.parent / "mwcc")
ap.add_argument("--wibo", type=Path, default=None, help="i686 Linux wibo (default: ../mwcc/build/cmenu-vn-state/wibo-i686)")
ap.add_argument("--out", type=Path, default=SFA / "build/model-skinning-fpr")
opt = ap.parse_args()
mwcc = opt.mwcc.resolve()
out = opt.out.resolve()
out.mkdir(parents=True, exist_ok=True)
wibo = (opt.wibo or mwcc / "build/cmenu-vn-state/wibo-i686").resolve()
(out / "wibo-i686").write_bytes(wibo.read_bytes())
os.chmod(out / "wibo-i686", 0o755)

import hashlib
assert hashlib.sha256((mwcc / "orig/GC_1_3/mwcceppc.exe").read_bytes()).hexdigest() == COMPILER_SHA
assert hashlib.sha256((SFA / "build/compilers/GC/1.3/mwcceppc.exe").read_bytes()).hexdigest() == COMPILER_SHA

ref = (SFA / "docs/foreign/model_skinning_reference.c").read_text()
body = ref[ref.index("/* Skinning reads the same live quantization state"):]
(out / "probe.c").write_text('#include "main/model.h"\n#include "dolphin/os/OSFastCast.h"\n\n' + body)

cmd = subprocess.run(["ninja", "-t", "commands", "build/GSAE01/src/main/model.o"], cwd=SFA,
                     capture_output=True, text=True, check=True).stdout.strip().splitlines()[-1]
args = shlex.split(cmd.split("&&")[0])
args = args[args.index("build/compilers/GC/1.3/mwcceppc.exe") + 1:]
flags = [a for a in args[:args.index("-c")] if a != "-MMD"]


def docker(extra, obj):
    return ["docker", "run", *extra, "--network", "none", "--read-only", "--cap-drop", "ALL",
            "--security-opt", "no-new-privileges", "--pids-limit", "64", "--memory", "768m", "--cpus", "2",
            "--tmpfs", "/tmp:rw,nosuid,size=128m",
            "--mount", f"type=bind,src={SFA},dst=/src,readonly", "--mount", f"type=bind,src={out},dst=/out",
            "--workdir", "/src", "--entrypoint", "/usr/bin/qemu-i386", IMAGE]


tail = ["/out/wibo-i686", "/src/build/compilers/GC/1.3/mwcceppc.exe", *flags, "-c", "/out/probe.c", "-o"]
subprocess.run(docker(["--rm"], None) + tail + ["/out/ordinary.o"], check=True)
subprocess.run(["docker", "rm", "-f", NAME], capture_output=True)
subprocess.run(docker(["-d", "--name", NAME], None) + ["-g", "12345"] + tail + ["/out/traced.o"],
               check=True, stdout=subprocess.DEVNULL)

server = socket.socket()
server.bind(("127.0.0.1", 0))
server.listen(1)
port = server.getsockname()[1]
inner = ("import socket,os,threading,time\nfor _ in range(200):\n try:\n  s=socket.create_connection(('127.0.0.1',12345));break\n"
         " except OSError:time.sleep(.05)\ns.setsockopt(socket.IPPROTO_TCP,socket.TCP_NODELAY,1)\n"
         "def send():\n while True:\n  b=os.read(0,65536)\n  if not b:break\n  s.sendall(b)\n"
         "threading.Thread(target=send,daemon=True).start()\nwhile True:\n b=s.recv(65536)\n if not b:break\n os.write(1,b)")


def relay():
    c, _ = server.accept()
    q = subprocess.Popen(["docker", "exec", "-i", NAME, "python3", "-c", inner], stdin=subprocess.PIPE, stdout=subprocess.PIPE)

    def send():
        try:
            while True:
                b = c.recv(65536)
                if not b:
                    break
                q.stdin.write(b)
                q.stdin.flush()
        except (OSError, BrokenPipeError):
            pass

    threading.Thread(target=send, daemon=True).start()
    try:
        while True:
            b = os.read(q.stdout.fileno(), 65536)
            if not b:
                break
            c.sendall(b)
    except OSError:
        pass
    finally:
        q.terminate()
        c.close()


threading.Thread(target=relay, daemon=True).start()
trace = out / "fpr_trace.json"
lldb = ["lldb", "--batch", "-o", f"target create {mwcc}/orig/GC_1_3/mwcceppc.exe", "-o", f"gdb-remote {port}",
        "-o", f"command script import {Path(__file__).with_name('fpr_lldb.py')}",
        "-o", "command script add -f fpr_lldb.run fprtrace", "-o", f"fprtrace {trace}"]
try:
    r = subprocess.run(lldb, capture_output=True, text=True, timeout=600)
    (out / "lldb.log").write_text(r.stdout + r.stderr)
    assert r.returncode == 0, r.stdout[-2000:]
finally:
    subprocess.run(["docker", "rm", "-f", NAME], capture_output=True)

assert (out / "traced.o").read_bytes() == (out / "ordinary.o").read_bytes(), "traced object differs"
rows = collections.OrderedDict()
cur = None
for e in json.loads(trace.read_text()):
    if e["kind"].endswith("entry"):
        cur = (e["function"], e["kind"])
        rows.setdefault(cur, dict(n=e["fpr_count"], regs=[]))
    elif e["opcode"] in ("0x9a", "0x197", "0x199", "0x193", "0x195"):
        rows[cur]["regs"].append((e["opcode"], e["args"][1]))
ok = True
for (fn, kind), r in rows.items():
    regs = sorted({reg for _, reg in r["regs"]}, reverse=True)
    anchored = regs == list(range(31, 31 - r["n"], -1))
    ok &= anchored
    print(f"{fn:45s} {kind:14s} N={r['n']:2d} regs={regs} {'f31-anchored' if anchored else 'NOT ANCHORED'}")
print("traced object identical to ordinary compile")
raise SystemExit(0 if ok else 1)
