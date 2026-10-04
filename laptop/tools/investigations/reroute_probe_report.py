import sys
import re,glob,os,random
S=sys.argv[1] if len(sys.argv)>1 else "rr/runs"   # directory holding one sub-directory per run (see docs)
def get(c):
    out={}
    for d in glob.glob(f"{S}/{c}/*"):
        if not os.path.exists(d+"/ok"): continue
        t=open(d+"/diag.log").read()
        m=re.search(r"RRDIAG (.*)",t); kv=dict((a,int(b)) for a,b in re.findall(r"(\w+)=(\d+)",m.group(1))) if m else {}
        kv["inc"]=sum(int(x) for x in re.findall(r"incidents_resolved=(\d+)",t))
        mm=re.search(r"victims_lost=(\d+)",t); kv["lost"]=int(mm.group(1)) if mm else 0
        out[int(os.path.basename(d))]=kv
    return out
off=get("blind_off"); on=get("blind_reroute")
ks=sorted(set(off)&set(on)); n=len(ks); print("seeds",n)
def mean(v): return sum(v)/len(v)
def ci(v,N=10000):
    random.seed(0); m=sorted(sum(random.choice(v) for _ in v)/len(v) for _ in range(N)); return m[int(.025*N)],m[int(.975*N)]
for lab,f in (("incidents resolved",lambda r:r['inc']),("victims lost",lambda r:r['lost'])):
    d=[f(on[k])-f(off[k]) for k in ks]; a,b=ci(d); print(f"{lab}: off {mean([f(off[k]) for k in ks]):.2f}, reroute {mean([f(on[k]) for k in ks]):.2f}, diff {mean(d):+.2f} [{a:+.2f}, {b:+.2f}]")
def tot(r,key): return mean([r[k][key] for k in ks])
print("\nrealised, per seed (mean):")
for lab,r in (("reroute OFF",off),("reroute ON ",on)):
    att=r[ks[0]] and mean([r[k]['attempts_rr']+r[k]['attempts_other'] for k in ks]); st=mean([r[k]['stalls_rr']+r[k]['stalls_other'] for k in ks])
    print(f"  {lab}: step attempts {att:8.1f}, stall ticks {st:7.1f}  (stall share of step attempts {100*st/att:.2f}%)")
d_att=[ (on[k]['attempts_rr']+on[k]['attempts_other'])-(off[k]['attempts_rr']+off[k]['attempts_other']) for k in ks]
d_st=[ (on[k]['stalls_rr']+on[k]['stalls_other'])-(off[k]['stalls_rr']+off[k]['stalls_other']) for k in ks]
a,b=ci(d_att); c,d=ci(d_st)
print(f"  paired diff: step attempts {mean(d_att):+.1f} [{a:+.1f}, {b:+.1f}], stall ticks {mean(d_st):+.1f} [{c:+.1f}, {d:+.1f}]")
print("\nwhile reroute is ON (reroute run): step attempts %.1f, stall ticks %.1f"%(tot(on,'attempts_rr'),tot(on,'stalls_rr')))
eps=tot(on,'episodes'); 
print(f"\nreroute episodes/seed measured at start: {eps:.1f}  (unreachable/unmeasured {tot(on,'unreachable'):.1f}); route unchanged by the overlay: {tot(on,'same_route'):.1f} ({100*tot(on,'same_route')/max(eps,1e-9):.0f}%)")
print(f"  per episode: original route {tot(on,'sumL0')/eps:.1f} cells with {tot(on,'sumP0')/eps:.2f} partial-debris cells; rerouted {tot(on,'sumL1')/eps:.1f} cells with {tot(on,'sumP1')/eps:.2f}")
dl=(tot(on,'sumL1')-tot(on,'sumL0'))/eps; dp=(tot(on,'sumP0')-tot(on,'sumP1'))/eps
print(f"  detour: {dl:+.2f} cells/episode; stalls avoided: {dp:.2f} cells x 4 ticks = {4*dp:.2f} ticks/episode; net est. {4*dp-dl:+.2f} ticks/episode")
