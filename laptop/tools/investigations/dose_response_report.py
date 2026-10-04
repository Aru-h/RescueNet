import sys
import re,glob,os,random,sys
S=sys.argv[1] if len(sys.argv)>1 else "dose/runs"   # directory holding one sub-directory per run (see docs)
def inc(d): return sum(int(x) for x in re.findall(r"incidents_resolved=(\d+)",open(d+"/diag.log").read()))
def lost(d):
    m=re.search(r"victims_lost=(\d+)",open(d+"/diag.log").read()); return int(m.group(1)) if m else 0
def runs(c,f): return {int(os.path.basename(d)):f(d) for d in glob.glob(f"{S}/{c}/*") if os.path.exists(d+"/ok")}
def ci(v,n=10000):
    random.seed(0); m=sorted(sum(random.choice(v) for _ in v)/len(v) for _ in range(n)); return m[int(.025*n)],m[int(.975*n)]
print("persistence   n  baseline_inc  oracle_inc  oracle-baseline [95% CI]   | baseline_lost  oracle_lost  diff [CI]")
for p in ["20-60","200-600","600-2400"]:
    bi=runs(f"ai_ship_{p}",inc); oi=runs(f"oracle_env_{p}",inc); bl=runs(f"ai_ship_{p}",lost); ol=runs(f"oracle_env_{p}",lost)
    ks=sorted(set(bi)&set(oi))
    if not ks: print(p,"no data"); continue
    di=[oi[k]-bi[k] for k in ks]; dl=[ol[k]-bl[k] for k in ks]
    a,b=ci(di); c,d=ci(dl); f=lambda v:sum(v)/len(v)
    print(f"{p:11s} {len(ks):3d} {f([bi[k] for k in ks]):10.2f} {f([oi[k] for k in ks]):10.2f} {f(di):+10.2f} [{a:+.2f}, {b:+.2f}] | {f([bl[k] for k in ks]):10.2f} {f([ol[k] for k in ks]):10.2f} {f(dl):+8.2f} [{c:+.2f}, {d:+.2f}]")
