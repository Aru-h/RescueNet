import sys
import re,glob,os,collections,sys
S=sys.argv[1] if len(sys.argv)>1 else "spec/runs"   # directory holding one sub-directory per run (see docs)
NAMES=["SEARCH","MEDICAL","EXTRACTION","PERIMETER","SUPPORT"]
seeds=sorted(int(os.path.basename(d)) for d in glob.glob(S+"/*") if os.path.exists(d+"/ok"))
n=len(seeds); print("seeds:",n, seeds[0], seeds[-1])
def parse(sd):
    d={}
    for line in open(f"{S}/{sd}/diag.log"):
        if line.startswith("SD_"):
            p=line.split(); vals=[int(x) for x in p[1:]]
            if p[0] in ("SD_Q0","SD_Q1","SD_UTIL","SD_WAIT","SD_CENS","SD_STAGEBY"): vals=vals[1:]   # drop the leading spec index
            d.setdefault(p[0],[]).append(vals)
    t=open(f"{S}/{sd}/diag.log").read()
    d["inc"]=sum(int(x) for x in re.findall(r"incidents_resolved=(\d+)",t))
    m=re.search(r"victims_lost=(\d+)",t); d["lost"]=int(m.group(1)) if m else 0
    return d
D={sd:parse(sd) for sd in seeds}
def pct(v,q):
    v=sorted(v); 
    if not v: return float('nan')
    return v[min(len(v)-1,int(q*len(v)))] if q<1 else v[-1]
def hist_stats(h):
    tot=sum(h); mean=sum(i*c for i,c in enumerate(h))/tot
    cum=0; p90=0
    for i,c in enumerate(h):
        cum+=c
        if cum>=0.9*tot: p90=i;break
    nz=1-h[0]/tot
    return mean,p90,nz
def mean(v): return sum(v)/len(v) if v else float('nan')
out=[]
print("\nchecks: mean incidents_resolved/seed =",round(mean([D[s]['inc'] for s in seeds]),2),"  mean victims_lost/seed =",round(mean([D[s]['lost'] for s in seeds]),2))
print("\n== open-incident queue (discovered, status OPEN, by CURRENT stage's required spec), pooled over %d seed-ticks"%(n*3000))
print("spec        organic mean  p90  %ticks>=1   | system-task open queue mean  p90")
for a in range(5):
    h0=[sum(D[s]["SD_Q0"][a][b] for s in seeds) for b in range(64)]; h1=[sum(D[s]["SD_Q1"][a][b] for s in seeds) for b in range(64)]
    m0,p0,z0=hist_stats(h0); m1,p1,z1=hist_stats(h1)
    print(f"{NAMES[a]:11s} {m0:7.2f} {p0:5d} {100*z0:8.1f}%   | {m1:7.2f} {p1:5d}")
print("\n== wait from becoming open (discovered / new stage / re-opened) to leaving OPEN, ticks (1 tick = 36 s); includes the 1-tick allocation cycle")
print("spec        episodes/seed  mean    p90   | still waiting at removal/end: per seed, mean age")
for a in range(5):
    w=[x for s in seeds for x in D[s]["SD_WAIT"][a]]; c=[x for s in seeds for x in D[s]["SD_CENS"][a]]
    print(f"{NAMES[a]:11s} {len(w)/n:9.1f} {mean(w):7.1f} {pct(w,0.9):6.0f}   | {len(c)/n:6.2f} {mean(c):8.1f}")
dl=[x for s in seeds for x in D[s]["SD_DLAG"][0]]
print(f"\nspawn-to-discovery (fog of war, all organic incidents): n/seed {len(dl)/n:.1f}, mean {mean(dl):.0f} ticks, p90 {pct(dl,0.9):.0f}")
print("\n== team utilisation by specialization (2 teams each), % of team-ticks")
print("spec         working  returning-to-rest  resting  available")
for a in range(5):
    r=[sum(D[s]["SD_UTIL"][a][i] for s in seeds) for i in range(5)]
    tt=r[0]
    print(f"{NAMES[a]:11s} {100*r[1]/tt:7.1f}% {100*r[2]/tt:12.1f}% {100*r[3]/tt:12.1f}% {100*r[4]/tt:9.1f}%")
print("\n== work per specialization (mean per seed)")
print("spec         stages done (organic) | system-task stages | resolved incidents (by archetype spec) | victims seen | victims lost (archetype) | lost (stage at death) | alive+unresolved at end")
def M(tag,a): return mean([D[s][tag][0][a] for s in seeds])
for a in range(5):
    print(f"{NAMES[a]:11s} {M('SD_STAGEORG',a):9.1f} {M('SD_STAGESYS',a):18.1f} {M('SD_RESARCH',a):26.1f} {M('SD_SEEN_ARCH',a):18.1f} {M('SD_DEATHS_ARCH',a):16.2f} {M('SD_DEATHS_STAGE',a):17.2f} {M('SD_VICEND_ARCH',a):16.1f}")
print("\n== who finished each stage (mean stages per seed): rows = spec the stage required, columns = spec of the team that finished it")
print("required\\done-by  "+"  ".join(f"{x[:6]:>7s}" for x in NAMES))
for a in range(5):
    row=[mean([D[s]["SD_STAGEBY"][a][b] for s in seeds]) for b in range(5)]
    print(f"{NAMES[a]:16s} "+"  ".join(f"{x:7.1f}" for x in row))
