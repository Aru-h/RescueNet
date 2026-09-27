"""
Generates core/feature_spec.h and feature_spec.py from feature_spec.json -
the single source of truth data_prep.py writes (its own module docstring
explains why). Never hand-edit the two generated files; re-run this script
after feature_spec.json changes. `make check-feature-spec-fresh` fails the
build if they've drifted apart.

usage: generate_feature_spec.py [feature_spec.json]
"""
import json
import re
import sys


def c_ident(name: str) -> str:
    """Feature/vocabulary name -> a valid, all-caps C identifier suffix."""
    return re.sub(r"[^A-Za-z0-9]", "_", name).upper()


def find_vocab_base(inputs, group: str, categories) -> int:
    """Index of the one-hot column for this group's FIRST category value -
    the block's base offset in the 74-input vector. group="team_zone_id",
    categories[0]=0 -> looks for "team_zone_id_0"."""
    target = f"{group}_{categories[0]}"
    if target not in inputs:
        raise ValueError(f"vocabulary group {group!r} has no column {target!r} "
                          f"in feature_spec.json's inputs - inputs/vocabularies "
                          f"are out of sync.")
    base = inputs.index(target)
    # Every category must occupy the contiguous block starting there, in
    # the same order pd.get_dummies()/pd.Categorical(categories=...) emits
    # them (data_prep.py's FIXED_VOCABULARIES) - checked, not assumed.
    for i, cat in enumerate(categories):
        expected = f"{group}_{cat}"
        if inputs[base + i] != expected:
            raise ValueError(f"vocabulary group {group!r}: expected {expected!r} "
                              f"at index {base + i}, found {inputs[base + i]!r} - "
                              f"inputs/vocabularies are out of sync.")
    return base


def generate_header(spec: dict) -> str:
    inputs = spec["inputs"]
    vocabularies = spec["vocabularies"]
    n = len(inputs)

    lines = [
        "/* GENERATED FILE - do not hand-edit.",
        " * Produced by generate_feature_spec.py from feature_spec.json.",
        " * Re-run that script after feature_spec.json changes; `make",
        " * check-feature-spec-fresh` fails the build if this has drifted. */",
        "#ifndef FEATURE_SPEC_H",
        "#define FEATURE_SPEC_H",
        "",
        f"#define FEATURE_SPEC_NUM_INPUTS {n}",
        "",
        "/* Index of each input in the 74-float vector core/fault_predictor.c",
        " * builds (core/feature_spec.h's own single source of truth for the",
        " * order - see data_prep.py's feature_columns list). */",
    ]
    for i, name in enumerate(inputs):
        lines.append(f"#define FEATURE_SPEC_IDX_{c_ident(name)} {i}")

    lines.append("")
    lines.append("/* One-hot vocabulary blocks: BASE is the index of the first")
    lines.append(" * category's column, COUNT how many contiguous columns follow.")
    lines.append(" * MIN is the vocabulary's first category VALUE (only differs")
    lines.append(" * from 0 for incident_req_capability's -1 \"none\" category). */")
    for group, categories in vocabularies.items():
        base = find_vocab_base(inputs, group, categories)
        ident = c_ident(group)
        lines.append(f"#define FEATURE_SPEC_{ident}_BASE {base}")
        lines.append(f"#define FEATURE_SPEC_{ident}_COUNT {len(categories)}")
        lines.append(f"#define FEATURE_SPEC_{ident}_MIN ({categories[0]})")

    lines.append("")
    lines.append("#include <stdint.h>")
    lines.append("")
    lines.append("/* Exported scaler mean/std (Decision: never recompute downstream -")
    lines.append(" * see data_prep.py's module docstring). Scaled input i is")
    lines.append(" * (raw[i] - FEATURE_SPEC_SCALER_MEAN[i]) / FEATURE_SPEC_SCALER_STD[i]. */")
    lines.append(f"static const float FEATURE_SPEC_SCALER_MEAN[FEATURE_SPEC_NUM_INPUTS] = {{")
    lines.append("    " + ", ".join(f"{v:.8f}f" for v in spec["scaler_mean"]))
    lines.append("};")
    lines.append(f"static const float FEATURE_SPEC_SCALER_STD[FEATURE_SPEC_NUM_INPUTS] = {{")
    lines.append("    " + ", ".join(f"{v:.8f}f" for v in spec["scaler_std"]))
    lines.append("};")
    lines.append("")
    lines.append("/* twin_remaining_path_cost's unreachable-sentinel (65535) replacement -")
    lines.append(" * the training-set mean of actively-traveling rows (data_prep.py). */")
    lines.append(f"#define FEATURE_SPEC_PATH_COST_SENTINEL_REPLACEMENT ({spec['path_cost_sentinel_replacement']:.8f}f)")
    lines.append("")
    lines.append("#endif /* FEATURE_SPEC_H */")
    lines.append("")
    return "\n".join(lines)


def generate_python(spec: dict) -> str:
    lines = [
        "# GENERATED FILE - do not hand-edit.",
        "# Produced by generate_feature_spec.py from feature_spec.json.",
        "# Re-run that script after feature_spec.json changes; `make",
        "# check-feature-spec-fresh` fails the build if this has drifted.",
        "",
        f"NUM_INPUTS = {len(spec['inputs'])}",
        "",
        "INPUTS = " + json.dumps(spec["inputs"], indent=4),
        "",
        "VOCABULARIES = " + json.dumps(spec["vocabularies"], indent=4),
        "",
        "SCALER_MEAN = " + json.dumps(spec["scaler_mean"]),
        "",
        "SCALER_STD = " + json.dumps(spec["scaler_std"]),
        "",
        f"PATH_COST_SENTINEL_REPLACEMENT = {spec['path_cost_sentinel_replacement']!r}",
        "",
    ]
    return "\n".join(lines)


def main():
    spec_path = sys.argv[1] if len(sys.argv) > 1 else "feature_spec.json"
    with open(spec_path) as f:
        spec = json.load(f)

    if len(spec["inputs"]) != len(spec["scaler_mean"]) or len(spec["inputs"]) != len(spec["scaler_std"]):
        raise ValueError("feature_spec.json: inputs/scaler_mean/scaler_std length mismatch")

    header = generate_header(spec)
    with open("core/feature_spec.h", "w") as f:
        f.write(header)
    print(f"wrote core/feature_spec.h ({len(spec['inputs'])} inputs)")

    py = generate_python(spec)
    with open("feature_spec.py", "w") as f:
        f.write(py)
    print("wrote feature_spec.py")


if __name__ == "__main__":
    main()
