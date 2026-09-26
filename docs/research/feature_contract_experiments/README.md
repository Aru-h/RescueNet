# feature_contract_experiments — THROWAWAY

Experiment scripts for `docs/research/feature_contract_research.md` (agent G,
research phase). **Throwaway**: they exist to reproduce the numbers in that
document, are not part of the build, and are not maintained. None of them
modify any tracked file; everything they write goes to `$TRON_G_WORK`
(default `./work`, outside version control).

Order (see the research doc, section "Reproduction", for expected output):

| Step | Script | Produces |
|---|---|---|
| 0 | `00_gen_dataset.sh` | 40-seed `tron_snapshot_dataset.csv` (patched **scratch copy** of the sim that adds one `heartbeat_emitted` column) |
| 1 | `01_prepare.py` | runs a text-patched **copy** of `data_prep-2.py` on that CSV (same features, same run_id split) and saves raw + scaled matrices and the split indices |
| 2 | `02_evidence_parity.py` | Python-mirror vs C-watchdog parity on the same CSV |
| 3 | `03_wire_format.py` | per-feature range/step/entropy, packed-format byte counts, airtime |
| 4 | `04_bottleneck.py` | full-input vs split-bottleneck vs PCA vs autoencoder vs scores-only vs packed |
| 5 | `05_peer_head.py` | peer-side head with self-report + peer observations, corrupted-report tests |

Python deps: `torch numpy pandas scikit-learn` (a scratch venv is enough;
the repo `.venv` has no torch).
