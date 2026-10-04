import csv,sys,collections,re
ST=["AVAIL","ENROUTE","ONSCENE","RESTING"]; AC=["CONT","LOCAL","RTB","REALLOC","ABORT"]
def run(d):
    inc=sum(int(x) for x in re.findall(r"incidents_resolved=(\d+)",open(d+"/diag.log").read()))
    rows=list(csv.DictReader(open(d+"/trace.csv")))
    n=len(rows); teams=sorted({int(r['team']) for r in rows}); ticks=max(int(r['tick']) for r in rows)
    print(f"== {d}  incidents_resolved={inc}  team-ticks={n}")
    # status occupancy
    occ=collections.Counter(ST[int(r['post_status'])] for r in rows)
    print("  post-dispatch status share:", {k:f"{100*v/n:.1f}%" for k,v in occ.items()})
    envon=[r for r in rows if r['env']=='1']
    print(f"  env flag on: {100*len(envon)/n:.1f}% of team-ticks")
    # recommendation while env on / off
    for lab,sub in (("env ON",envon),("env OFF",[r for r in rows if r['env']=='0'])):
        c=collections.Counter(AC[int(r['rec'])] for r in sub)
        m=len(sub) or 1
        print(f"  rec ({lab}, n={len(sub)}):", {k:f"{100*v/m:.1f}%" for k,v in c.most_common()})
        s=collections.Counter(ST[int(r['post_status'])] for r in sub)
        print(f"  status ({lab}):", {k:f"{100*v/m:.1f}%" for k,v in s.most_common()})
    # reasons when rec != CONT
    c=collections.Counter((AC[int(r['rec'])],r['reason'],r['env']) for r in rows if r['rec']!='0')
    print("  non-CONT (action,reason,envflag) top:",c.most_common(8))
    # transitions caused by dispatch (pre!=post) by action
    t=collections.Counter((AC[int(r['rec'])],ST[int(r['pre_status'])]+'>'+ST[int(r['post_status'])]) for r in rows if r['pre_status']!=r['post_status'] and r['rec']!='0')
    print("  dispatch-caused status changes:",t.most_common(8))
    # per team: ticks env on, ticks resting, ticks working (on scene/en route with task)
    print("  team spec envON% rest% avail% enroute% onscene%")
    for tm in teams:
        rr=[r for r in rows if int(r['team'])==tm]; k=len(rr)
        sp=rr[0]['spec']
        f=lambda cond: 100*sum(1 for r in rr if cond(r))/k
        print(f"   {tm} {sp}  {f(lambda r:r['env']=='1'):5.1f} {f(lambda r:r['post_status']=='3'):5.1f} {f(lambda r:r['post_status']=='0'):5.1f} {f(lambda r:r['post_status']=='1'):5.1f} {f(lambda r:r['post_status']=='2'):5.1f}")
    # episodes of env flag: length; and idle ticks during env (resting+avail)
    ep=[];cur=collections.defaultdict(int)
    for r in rows:
        k=int(r['team'])
        if r['env']=='1': cur[k]+=1
        elif cur[k]: ep.append(cur[k]); cur[k]=0
    for k,v in cur.items():
        if v: ep.append(v)
    if ep: print(f"  env-flag episodes: {len(ep)}, mean {sum(ep)/len(ep):.0f} ticks, max {max(ep)}")
for d in sys.argv[1:]: run(d)
