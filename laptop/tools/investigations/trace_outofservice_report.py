import csv,sys,collections,re
AC=["CONT","LOCAL","RTB","REALLOC","ABORT"]
def run(d):
    inc=sum(int(x) for x in re.findall(r"incidents_resolved=(\d+)",open(d+"/diag.log").read()))
    rows=list(csv.DictReader(open(d+"/trace.csv"))); n=len(rows)
    def cls(r):
        s=int(r['post_status']); rtb=r['rtb']=='1'
        if s==3: return "resting"
        if s==0: return "available"
        if rtb: return "returning-to-rest"
        return "working(en route/on scene)"
    c=collections.Counter(cls(r) for r in rows)
    print(f"== {d} incidents={inc}")
    print("  team-tick share:", {k:f"{100*v/n:.1f}%" for k,v in sorted(c.items(),key=lambda x:-x[1])})
    for lab,sub in (("env ON",[r for r in rows if r['env']=='1']),("env OFF",[r for r in rows if r['env']=='0'])):
        m=len(sub) or 1; cc=collections.Counter(cls(r) for r in sub)
        print(f"  {lab} (n={len(sub)}):",{k:f"{100*v/m:.1f}%" for k,v in sorted(cc.items(),key=lambda x:-x[1])})
    # dispatch counts of RTB actually changing state (pre!=post) and RTB recs
    rtb_rec=sum(1 for r in rows if r['rec']=='2'); 
    # number of distinct RTB "episodes" = RTB recommendation starts
    starts=0; prev={}
    for r in rows:
        k=int(r['team']); cur=r['rec']=='2'
        if cur and not prev.get(k): starts+=1
        prev[k]=cur
    print(f"  RTB recommendation: {rtb_rec} team-ticks in {starts} separate stretches (mean {rtb_rec/max(starts,1):.0f} ticks)")
    # rest cycle: RESTING->AVAILABLE promotions followed by re-return
    bounce=0; last={}
    for r in rows:
        k=int(r['team']); s=int(r['pre_status']); ps=last.get(k)
        if ps==3 and s!=3: bounce+=1
        last[k]=int(r['post_status'])
    print(f"  RESTING->(other) promotions before dispatch: {bounce}")
    # fraction of RESTING ticks in which stamina and equipment are already healthy (rest no longer needed)
    rest=[r for r in rows if r['post_status']=='3']
    healthy=[r for r in rest if int(r['stamina'])>=90 and int(r['eq'])>=90]
    print(f"  RESTING team-ticks with stamina>=90 and equipment>=90: {100*len(healthy)/max(len(rest),1):.1f}% ({len(healthy)}/{len(rest)})")
    # idle-in-rest while env ON and rec==RTB and healthy
    held=[r for r in rest if r['env']=='1' and r['rec']=='2' and int(r['stamina'])>=90 and int(r['eq'])>=90]
    print(f"  healthy team-ticks held at the safe zone by an RTB recommendation (env flag on): {len(held)} = {100*len(held)/n:.1f}% of all team-ticks")
for d in sys.argv[1:]: run(d)
