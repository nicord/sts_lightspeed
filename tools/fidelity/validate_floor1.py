"""Runs fidelity_harness against a corpus of (seed, neow_choice_idx, floor1_x, floor1_cards)
tuples extracted by extract_floor1_corpus.py and reports exact-match rate for floor-1 card
rewards. Card identity comparison is by NAME (both ground truth `.id` and the harness's
card.getName() are the human-readable display name, e.g. "Twin Strike" -- verified consistent
in the original spike report's table), and treated as a SET (reward order/shuffle within the
3 offered slots is not itself a fidelity claim we're testing -- the pool-order bug is about
WHICH 3 cards, not their on-screen order) unless --ordered is passed.
"""
import json
import subprocess
import sys

HARNESS = "/private/tmp/claude-501/-Users-nico-git-spire-agent/d4e15dba-42f8-4fc2-b183-533b03dd427a/scratchpad/spike/fidelity_harness"


def run_one(seed, ascension, neow_idx, floor1_x):
    proc = subprocess.run(
        [HARNESS, seed, "I", str(ascension), str(neow_idx), str(floor1_x)],
        capture_output=True, text=True, timeout=10,
    )
    try:
        return json.loads(proc.stdout), proc.stderr
    except Exception as e:
        return None, f"PARSE_ERROR: {e}; stdout={proc.stdout[:300]}; stderr={proc.stderr[:300]}"


def main():
    corpus_path = sys.argv[1] if len(sys.argv) > 1 else "floor1_corpus_30.json"
    ordered = "--ordered" in sys.argv

    with open(corpus_path) as f:
        corpus = json.load(f)

    n_match = 0
    n_total = 0
    rows = []
    for r in corpus:
        sim, err = run_one(r["seed"], r["ascension"], r["neow_choice_idx"], r["floor1_x"])
        n_total += 1
        if sim is None:
            rows.append({"seed": r["seed"], "match": False, "error": err})
            continue
        rewards = sim.get("card_rewards", [])
        if not rewards:
            rows.append({"seed": r["seed"], "match": False, "error": "NO_REWARDS", "expected": r["floor1_cards"]})
            continue
        got = rewards[0]["cards"]
        expected = r["floor1_cards"]
        if ordered:
            match = got == expected
        else:
            match = sorted(got) == sorted(expected)
        if match:
            n_match += 1
        rows.append({
            "seed": r["seed"], "match": match,
            "expected": expected, "got": got,
            "floor_got": rewards[0].get("floor"),
        })

    print(json.dumps(rows, indent=1))
    print(f"\n=== {n_match}/{n_total} floor-1 card-reward matches ===", file=sys.stderr)


if __name__ == "__main__":
    main()
