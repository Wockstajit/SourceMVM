"""Where CS2 keeps the exposure its tonemapper applies, in the per-view constants of a lit draw.

Forces `mat_tonemap_force_scale` to a few values, dumps the engine's b1 and b4 constant buffers of a
world view each time (`mirv_relight dumpview`, into the mvm_debug log) and lists every lane whose
value changed, marking the ones that follow the forced scale. The last scale should be 0 (the
engine's own adaptation): that column is then the exposure CS2 chose itself.

The hook reads b1[18].x (`NativeExposureRow` in EnvironmentNativeMaterial.cpp) with .y as its
reciprocal. Run this after a CS2 update if `mirv_relight engineexposure` reports 0 in a lit map:

  python Automation/verify/find-engine-exposure.py [0.5 2 1 0]

Needs a loaded offline map or demo with Dynamic lighting on. Leaves the scale at 0 and stops mvm_debug.
"""
import glob, importlib.util, os, re, sys, time
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]
spec=importlib.util.spec_from_file_location("prm",ROOT/"Automation"/"verify"/"profile-relight-motion.py")
prm=importlib.util.module_from_spec(spec);spec.loader.exec_module(prm)
net=prm.Netcon(29010)
scales=[float(v) for v in (sys.argv[1:] or ["0.5","2","1","0"])]
net.command(["sv_cheats 1","mvm_debug start"],1.0)
marks=[]
for scale in scales:
    net.command(["mat_tonemap_force_scale %g"%scale],0.5)
    time.sleep(4)
    net.command(["mirv_relight dumpview"],0.5)
    time.sleep(2.5)
    marks.append(scale)
net.command(["mat_tonemap_force_scale 0","mvm_debug stop"],1.0)
time.sleep(0.5)
log=max(glob.glob(os.path.expandvars(r"%APPDATA%\HLAE\debuglogs\mvm_debug_*.log")),key=os.path.getmtime)
dumps=[];current=None;last_row=None
for line in open(log,encoding="utf-8",errors="replace"):
    m=re.search(r"\[relight\.view\.dump\].*?b(\d+)\[(\d+)\]=\(([^)]*)\)",line)
    if not m: continue
    b,row=int(m[1]),int(m[2])
    if b==1 and row==0:
        current={};dumps.append(current)
    if current is None: continue
    current[(b,row)]=[float(v) for v in m[3].split()]
print("log:",log,"dumps:",len(dumps),"scales:",marks)
if len(dumps)!=len(marks):
    print("dump count does not match"); sys.exit(1)
keys=sorted(set.intersection(*[set(d) for d in dumps]))
for key in keys:
    for lane in range(4):
        values=[d[key][lane] for d in dumps]
        if len(set(values))<=1: continue
        forced=[(s,v) for s,v in zip(marks,values) if s>0]
        ratio=[v/s for s,v in forced if s]
        follows=len(forced)>=2 and max(ratio)-min(ratio)<=1e-3*max(1e-9,abs(max(ratio,key=abs))) and all(v!=0 for _,v in forced)
        print("b%d[%d].%s %s%s"%(key[0],key[1],"xyzw"[lane]," ".join("%.6g"%v for v in values),"   <-- follows the forced scale (x%.6g)"%ratio[0] if follows else ""))
