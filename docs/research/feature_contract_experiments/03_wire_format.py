"""THROWAWAY. Step 3: wire-format design numbers.

* classifies every training feature as static / slow / per-tick from the data
* defines the quantiser (range, step, bits) per wire field and the receiver-side derivations
* writes work/prep/wire_spec.json + quantised val/test matrices for 04_bottleneck.py
* simulates absolute vs keyframe+delta encoding on the real per-team tick streams
* computes airtime with the repo's Semtech AN1200.13 formula (SF7/BW125/CR4/5, explicit
  header, CRC on, 8-symbol preamble, LDRO off) and the duty cycle at 100 ticks/hour
"""
import json, math, os
import numpy as np, pandas as pd

WORK = os.environ.get("TRON_G_WORK", os.path.abspath("work")); P = f"{WORK}/prep"
X = pd.read_pickle(f"{P}/X_raw.pkl"); M = pd.read_pickle(f"{P}/meta.pkl")
idx = np.load(f"{P}/split_idx.npz"); cols = list(X.columns)
SENTINEL_FILL = 49.04   # printed by data_prep-2.py; recomputed below from train rows

# ------------------------------------------------------------------ airtime (AN1200.13)
def toa_ms(wire_len, sf=7, bw=125000, cr=1, preamble=8, crc=True, explicit=True, ldro=False):
    tsym = (2 ** sf) / bw * 1000
    ih = 0 if explicit else 1
    n = 8 * wire_len - 4 * sf + 28 + (16 if crc else 0) - (20 if ih else 0)
    d = 4 * (sf - 2 * (1 if ldro else 0))
    npay = 8 + max(math.ceil(n / d) * (cr + 4), 0)
    return (preamble + 4.25) * tsym + npay * tsym
assert abs(toa_ms(80) - 143.616) < 1e-6, toa_ms(80)   # vbus.h / docs/architecture/CONSENSUS_DESIGN.md reference value
FRAME_OVERHEAD = 7 + 21   # LORA_FRAME_HEADER_SIZE + LORA_FRAME_TRAILER_SIZE (core/lora_frame.h)

# ------------------------------------------------------------------ change rates per feature
key = M[["run_id", "team_id"]].astype(np.int64).to_numpy()
same = np.r_[False, (key[1:] == key[:-1]).all(1)]        # row i has a predecessor (same run, team; cycle+1)
Xv = X.to_numpy(dtype=np.float64)
chg = (Xv[1:] != Xv[:-1]) & same[1:, None]
rate = chg.sum(0) / same[1:].sum()
static = {}
g = pd.DataFrame(Xv, columns=cols); g["run_id"] = key[:, 0]; g["team_id"] = key[:, 1]
n_vals_per_team = g.groupby(["run_id", "team_id"])[cols].nunique().max()
rows = []
for c, r in zip(cols, rate):
    cls = "static" if n_vals_per_team[c] <= 1 else ("slow" if r < 0.05 else "per-tick")
    rows.append((c, r, cls))
chg_df = pd.DataFrame(rows, columns=["feature", "change_rate", "class"])
chg_df.to_csv(f"{WORK}/feature_change_rates.csv", index=False)
print("\n== change rate classes (share of 72 columns):"); print(chg_df["class"].value_counts().to_string())

# ------------------------------------------------------------------ quantiser
def q_uniform(a, lo, hi, step):
    return np.clip(np.round((a - lo) / step) * step + lo, lo, hi)

def quantise_frame(Xf, evid_bits=8):
    """Receiver-side reconstruction of the 72 training features from the wire fields."""
    o = Xf.copy()
    st = q_uniform(Xf["stamina"].to_numpy(), 0, 100, 0.5)
    se = q_uniform(Xf["twin_stamina_error"].to_numpy(), -64, 63.75, 0.25)
    o["stamina"] = st; o["twin_actual_stamina"] = st
    o["twin_stamina_error"] = se; o["twin_expected_stamina"] = np.clip(st + se, 0, 100)
    ap = q_uniform(Xf["twin_actual_progress"].to_numpy(), 0, 1, 1 / 255)
    pe = q_uniform(Xf["twin_progress_error"].to_numpy(), -1, 1 - 1 / 128, 1 / 128)
    o["twin_actual_progress"] = ap; o["twin_progress_error"] = pe
    o["twin_expected_progress"] = np.clip(ap + pe, 0, 1)
    unreach = Xf["path_is_unreachable"].to_numpy() == 1
    cost = np.clip(np.round(Xf["twin_remaining_path_cost"].to_numpy()), 0, 1022)
    o["twin_remaining_path_cost"] = np.where(unreach, SENTINEL_FILL, cost)
    o["twin_expected_work_remaining"] = np.clip(Xf["twin_expected_work_remaining"], 0, 1023)
    o["incident_priority"] = np.clip(Xf["incident_priority"], -1, 6)
    o["victim_count"] = np.clip(Xf["victim_count"], -1, 30)
    o["hours_worked"] = np.clip(Xf["hours_worked"], 0, 31)
    o["env_raw_travel_cost"] = np.clip(Xf["env_raw_travel_cost"], 0, 63)
    n = 2 ** evid_bits - 1
    for ch in [c for c in Xf.columns if c.startswith("evidence_")]:
        o[ch] = np.round(np.clip(Xf[ch].to_numpy(), 0, 1) * n) / n
    return o

train_mask = M["split"].to_numpy() == 0
real = (X["path_is_unreachable"] == 0) & (X["twin_remaining_path_cost"] > 0)
import pickle, sys
class SafeStandardScaler:            # stub so data_prep-2.py's pickled scaler can be loaded
    pass
sys.modules["__main__"].SafeStandardScaler = SafeStandardScaler
sc = pickle.load(open(f"{P}/scaler.pkl", "rb"))   # the scaler data_prep-2.py itself fitted
mean, std = np.asarray(sc.mean_, dtype=np.float32), np.asarray(sc.std_, dtype=np.float32)
# check against the scaled matrix data_prep-2.py itself saved
Xtr_saved = np.load(f"{P}/X_train_scaled.npy", mmap_mode="r")
chk = ((X.to_numpy(dtype=np.float32)[idx["train"][:1000]] - mean) / std - Xtr_saved[:1000])
print("scaler reconstruction max |diff| vs saved X_train_scaled:", float(np.abs(chk).max()), "at column", cols[int(np.abs(chk).max(0).argmax())])
for b in (8, 6):
    Q = quantise_frame(X, evid_bits=b)
    for name in ("val", "test"):
        a = ((Q.to_numpy(dtype=np.float32)[idx[name]] - mean) / std).astype(np.float32)
        np.save(f"{P}/Xq{b}_{name}_scaled.npy", a)
    err = (Q.to_numpy(dtype=np.float64) - X.to_numpy(dtype=np.float64))
    print(f"\nquantisation error, evidence_bits={b}: max |err| per numeric column (raw units)")
    for c in ["stamina", "twin_expected_stamina", "twin_stamina_error", "twin_expected_progress", "twin_actual_progress",
              "twin_progress_error", "twin_remaining_path_cost", "evidence_progress", "evidence_communication"]:
        print("  %-26s %.5f" % (c, np.abs(err[:, cols.index(c)]).max()))

# ------------------------------------------------------------------ wire fields (bits)
one_hot = {"specialization": 5, "status": 4, "twin_expected_operating_state": 4, "fitness_tier": 3,
           "incident_req_capability": 5, "support_team": 11, "team_zone_id": 10}
radix_bits = math.ceil(math.log2(np.prod(list(one_hot.values()))))
fields = [  # name, bits, class(static/slow/per-tick), note
    ("team_id", 4, "static", "0..9 (header, not a feature)"),
    ("specialization|fitness|capability... (mixed radix 5*4*4*3*5*11*10)", radix_bits, "mixed", "7 categorical features"),
    ("equipment bits (8) + hazard bits (4)", 12, "slow", "equipment_flags static; hazard_flags follows the incident"),
    ("equipment_level", 7, "slow", "0..100"), ("team_zone_trust", 7, "slow", "0..100"),
    ("hours_worked", 5, "slow", "0..31 sat."), ("incident_priority", 3, "slow", "-1..6"), ("victim_count", 5, "slow", "-1..30 sat."),
    ("twin_mission_stage", 2, "per-tick", ""), ("twin_communication_health", 2, "per-tick", "0..3"),
    ("stamina", 8, "per-tick", "0..100 step 0.5"), ("twin_stamina_error", 9, "per-tick", "-64..63.75 step 0.25"),
    ("twin_actual_progress", 8, "per-tick", "0..1 step 1/255"), ("twin_progress_error", 8, "per-tick", "-1..1 step 1/128"),
    ("twin_remaining_path_cost", 10, "per-tick", "0..1022 + 1023=unreachable"),
    ("twin_expected_work_remaining", 10, "per-tick", "0..1023"), ("env_raw_travel_cost", 6, "per-tick", "0..63 sat."),
    ("evidence_* x7", 7 * 8, "per-tick", "8 bit, step 1/255"), ("confirmed_mobility_impairment", 1, "per-tick", ""),
]
def total(fs, evid=8):
    return sum(b if not n.startswith("evidence_") else 7 * evid for n, b, *_ in fs)
abs_bits = {b: total(fields, b) for b in (8, 6, 5)}
print("\n== absolute record bits (evidence bits 8/6/5):", abs_bits, " bytes:", {k: math.ceil(v / 8) for k, v in abs_bits.items()})
tick_only = sum(b for n, b, c, _ in fields if c == "per-tick")
print("per-tick fields only:", tick_only, "bits =", math.ceil(tick_only / 8), "B; static+slow:", abs_bits[8] - tick_only, "bits")

# ------------------------------------------------------------------ delta / keyframe simulation on the real streams
Q8 = quantise_frame(X, 8)
gq = {  # wire-level integer codes per field group, from the reconstructed frame
    "mixed": [c for c in cols if c.startswith(tuple(f"{k}_" for k in one_hot))],
    "bits12": [c for c in cols if c.startswith(("equipment_", "hazard_")) and c != "equipment_level"],
    "equipment_level": ["equipment_level"], "team_zone_trust": ["team_zone_trust"], "hours_worked": ["hours_worked"],
    "incident_priority": ["incident_priority"], "victim_count": ["victim_count"],
    "twin_mission_stage": ["twin_mission_stage"], "twin_communication_health": ["twin_communication_health"],
    "stamina": ["stamina"], "twin_stamina_error": ["twin_stamina_error"], "twin_actual_progress": ["twin_actual_progress"],
    "twin_progress_error": ["twin_progress_error"], "twin_remaining_path_cost": ["twin_remaining_path_cost", "path_is_unreachable"],
    "twin_expected_work_remaining": ["twin_expected_work_remaining"], "env_raw_travel_cost": ["env_raw_travel_cost"],
    "evidence": [c for c in cols if c.startswith("evidence_")], "cmi": ["confirmed_mobility_impairment"],
}
gbits = {"mixed": radix_bits, "bits12": 12, "equipment_level": 7, "team_zone_trust": 7, "hours_worked": 5, "incident_priority": 3,
         "victim_count": 5, "twin_mission_stage": 2, "twin_communication_health": 2, "stamina": 8, "twin_stamina_error": 9,
         "twin_actual_progress": 8, "twin_progress_error": 8, "twin_remaining_path_cost": 10, "twin_expected_work_remaining": 10,
         "env_raw_travel_cost": 6, "cmi": 1}
Qv = Q8.to_numpy(dtype=np.float64); ci = {c: i for i, c in enumerate(cols)}
gchg = {}
for g_, cs in gq.items():
    a = Qv[:, [ci[c] for c in cs]]
    gchg[g_] = np.r_[False, (a[1:] != a[:-1]).any(1)] & same
evid_ch = [Qv[:, [ci[c]]] for c in gq["evidence"]]
evid_chg = np.stack([np.r_[False, (a[1:, 0] != a[:-1, 0])] & same for a in evid_ch], 1)   # per-channel change
print("\n== per-group probability the quantised value changed vs previous tick (delta-encoding hit rate):")
for g_, v in gchg.items():
    print("  %-30s %.3f" % (g_, v[same].mean()))
print("  %-30s %s" % ("evidence per channel", np.round(evid_chg[same].mean(0), 3)))

n_groups = len(gbits) + 7          # 17 non-evidence groups + 7 evidence channels
def delta_bytes(K):
    """bitmap of n_groups changed-flags + payload of changed groups; keyframe (absolute) every K ticks."""
    bits = np.full(len(X), float(n_groups), dtype=np.float64)
    for g_, b in gbits.items():
        bits += gchg[g_] * b
    bits += evid_chg.sum(1) * 8
    tick = M["cycle"].to_numpy()
    key_row = (tick % K == 0) | ~same
    bits = np.where(key_row, abs_bits[8], bits)
    return bits[same] / 8.0
res = {}
print("\n== bytes per team per tick, absolute vs keyframe+delta (payload only, before 28 B frame overhead):")
print("  absolute: %.1f B" % (abs_bits[8] / 8))
for K in (5, 10, 20, 50):
    by = delta_bytes(K)
    res[K] = by
    print("  delta K=%-3d mean %.1f  p50 %.1f  p95 %.1f  max %.1f B" % (K, by.mean(), np.percentile(by, 50), np.percentile(by, 95), by.max()))

# ------------------------------------------------------------------ airtime and duty cycle
print("\n== airtime (SF7/BW125/CR4/5, 28 B frame overhead, 100 ticks/h => tick=36 s)")
print("  ref: 52 B consensus beacon (80 B wire): %.3f ms, duty %.2f %%" % (toa_ms(80), toa_ms(80) * 100 / 36000))
def line(label, payload):
    w = payload + FRAME_OVERHEAD
    print("  %-46s payload %3d B wire %3d B  ToA %7.2f ms  duty(1 frame/tick) %.2f %%  1 frame/tick x10 nodes %.2f s" %
          (label, payload, w, toa_ms(w), toa_ms(w) * 100 / 36000, toa_ms(w) * 10 / 1000))
abs_B = math.ceil(abs_bits[8] / 8)
line("absolute record (1 team)", abs_B)
line("delta mean (K=10)", math.ceil(res[10].mean()))
line("delta p95 (K=10)", math.ceil(np.percentile(res[10], 95)))
line("keyframe only (per-tick fields only)", math.ceil(tick_only / 8) + 2)
line("scores only, 5 x u8", 5); line("split k=8 int8", 8); line("split k=16 int8", 16)
line("beacon 52 B + absolute record", 52 + abs_B); line("beacon 52 B + delta mean (K=10)", 52 + math.ceil(res[10].mean()))
line("beacon 52 B + delta p95 (K=10)", 52 + math.ceil(np.percentile(res[10], 95)))
line("board hosting 5 teams, 5 abs records", 5 * abs_B + 1); line("board hosting 5 teams, 5 delta mean", 5 * math.ceil(res[10].mean()) + 1)
line("max payload 144 B", 144)

spec = dict(features=cols, scaler=dict(mean=mean.tolist(), std=std.tolist()), sentinel_fill=float(X.loc[X.path_is_unreachable == 1, "twin_remaining_path_cost"].iloc[0]) if (X.path_is_unreachable == 1).any() else None,
            quant=dict(lo=[0] * len(cols), hi=[1] * len(cols), bits=[0] * len(cols)), static_plus_tick_bits=abs_bits[8],
            one_hot_radix=one_hot, fields=[dict(name=n, bits=b, cls=c, note=t) for n, b, c, t in fields])
json.dump(spec, open(f"{P}/wire_spec.json", "w"))
print("\nwrote wire_spec.json; sentinel fill value present in data:", spec["sentinel_fill"])
