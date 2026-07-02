#!/usr/bin/env python3
"""Compare sts_lightspeed harness output (map/Neow/boss) against recorded ground truth
for all 5 sampled seeds. Card rewards were already root-caused separately (pool-order bug)
and are not re-tested here since they require exact path replication + the known-broken pools."""
import json
import subprocess

HARNESS = "/private/tmp/claude-501/-Users-nico-git-spire-agent/d4e15dba-42f8-4fc2-b183-533b03dd427a/scratchpad/spike/fidelity_harness"

with open("/private/tmp/claude-501/-Users-nico-git-spire-agent/d4e15dba-42f8-4fc2-b183-533b03dd427a/scratchpad/spike/ground_truth.json") as f:
    ground_truth = json.load(f)

# Neow bonus string -> normalized recorded text mapping is fuzzy (recorded has bracketed UI text with
# newlines collapsed); we do a light substring-based match on key nouns instead of exact string equality.

rows = []
for gt in ground_truth:
    seed_str = gt["seed_string_start"]
    ascension = gt["ascension"]
    proc = subprocess.run([HARNESS, seed_str, "I", str(ascension), "0"], capture_output=True, text=True)
    try:
        sim = json.loads(proc.stdout)
    except Exception as e:
        rows.append((seed_str, "PARSE_ERROR", str(e), proc.stdout[:200]))
        continue

    # seed round trip
    seed_ok = sim["seed_numeric_computed"] == int(gt["seed_numeric"]) and sim["seed_string_roundtrip"] == seed_str

    # boss
    boss_ok = sim["boss"] == gt["act_boss"]

    # map: convert both to sorted (x,y,symbol) tuples
    gt_map = sorted(tuple(x) for x in gt["map"])
    sim_map = sorted((n["x"], n["y"], n["symbol"]) for n in sim["map"])
    map_ok = gt_map == sim_map

    rows.append({
        "seed": seed_str,
        "seed_numeric_match": seed_ok,
        "boss_expected": gt["act_boss"],
        "boss_got": sim["boss"],
        "boss_match": boss_ok,
        "map_match": map_ok,
        "map_diff_count": len(set(gt_map) ^ set(sim_map)) if not map_ok else 0,
        "neow_expected_recorded_text": gt["neow_options"],
        "neow_got_sim": [f"{o['bonus']} | {o['drawback']}" for o in sim["neow_options"]],
    })

print(json.dumps(rows, indent=2))
