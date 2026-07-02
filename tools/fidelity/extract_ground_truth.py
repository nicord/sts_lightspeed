#!/usr/bin/env python3
"""Extract ground-truth artifacts (seed, map, neow options, card rewards, boss)
from a recorded SpireAgent trajectory JSONL for comparison against sts_lightspeed."""
import json
import sys


def extract(path):
    with open(path) as f:
        lines = [json.loads(l) for l in f]

    out = {"file": path}

    # seed numeric (from start_game action) and seed string (from filename / start action)
    for d in lines:
        act = d.get("action")
        if act and act.get("type") == "start_game":
            out["seed_string_start"] = act.get("seed")
            out["ascension"] = act.get("ascension")
            break

    # numeric seed + map + boss from first game_state with a map
    for d in lines:
        gs = d.get("state", {}).get("game_state")
        if gs and "seed" in d.get("state", {}).get("game_state", {}):
            out["seed_numeric"] = gs["seed"]
            out.setdefault("act_boss", gs.get("act_boss"))
            break

    for d in lines:
        gs = d.get("state", {}).get("game_state")
        if gs and "map" in gs:
            nodes = gs["map"]
            # (x,y)->symbol
            out["map"] = sorted([(n["x"], n["y"], n["symbol"]) for n in nodes])
            out["boss_at_map_gen"] = gs.get("act_boss")
            break

    # Neow options (event screen at floor 0, second EVENT screen with 4 options)
    for d in lines:
        gs = d.get("state", {}).get("game_state")
        if gs and gs.get("screen_type") == "EVENT" and gs.get("floor") == 0:
            opts = gs.get("screen_state", {}).get("options")
            if opts and len(opts) == 4:
                out["neow_options"] = [o.get("text") for o in opts]
                break

    # first N card reward screens
    rewards = []
    for d in lines:
        gs = d.get("state", {}).get("game_state")
        if gs and gs.get("screen_type") == "CARD_REWARD":
            cards = gs.get("screen_state", {}).get("cards")
            if cards:
                rewards.append({"floor": gs.get("floor"), "cards": [c.get("id") for c in cards]})
        if len(rewards) >= 5:
            break
    out["card_rewards"] = rewards

    return out


if __name__ == "__main__":
    results = []
    for path in sys.argv[1:]:
        results.append(extract(path))
    print(json.dumps(results, indent=2))
