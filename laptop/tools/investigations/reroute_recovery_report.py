"""Legacy recovery activity with REROUTE off vs on (blind A*), from runs of the
reroute probe build (RRREC line). usage: reroute_recovery_report.py <runs2 dir>
(containing blind_off/<seed> and blind_reroute/<seed>)."""
import re, glob, os, random, sys
S = sys.argv[1]
def get(c):
    o = {}
    for d in glob.glob(f"{S}/{c}/*"):
        t = open(d + "/diag.log").read(); m = re.search(r"RRREC (.*)", t)
        o[os.path.basename(d)] = dict((a, int(b)) for a, b in re.findall(r"(\w+)=(\d+)", m.group(1)))
    return o
off = get("blind_off"); on = get("blind_reroute"); ks = sorted(set(off) & set(on)); n = len(ks)
def ci(v, N=10000):
    random.seed(0); m = sorted(sum(random.choice(v) for _ in v) / len(v) for _ in range(N)); return m[int(.025 * N)], m[int(.975 * N)]
names = {"a1": "LOCAL_RECOVERY", "a2": "RETURN_TO_SAFE_ZONE", "a3": "REQUEST_TASK_REALLOCATION", "a4": "ABORT_MISSION", "progress_dev_total": "of which reason=PROGRESS_DEVIATION"}
for k, l in names.items():
    d = [on[s][k] - off[s][k] for s in ks]; a, b = ci(d)
    print(f"{l:40s} {sum(off[s][k] for s in ks)/n:9.1f} -> {sum(on[s][k] for s in ks)/n:9.1f}  {sum(d)/n:+8.1f} [{a:+.1f}, {b:+.1f}]")
