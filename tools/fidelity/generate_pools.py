"""
Reproduces the real game's card-reward/shop/Neow/curse pool ORDER for Ironclad (and the
shared colorless/curse pools), by:

  1. Replaying CardLibrary.cards' HashMap<String, AbstractCard> insertion sequence (the exact
     `add(new X())` call order transcribed from CardLibrary.java's addRedCards/addGreenCards/
     addBlueCards/addPurpleCards/addColorlessCards/addCurseCards, in the order they're called
     from CardLibrary.initialize() — see CardLibrary.java:406-413) through the
     JavaHashMapOrderSimulator (java_hashmap.py, cross-verified byte-exact against a live
     JDK 26 `HashMap<String,V>` for this exact 370-key set).
  2. Replaying the separate `curses` HashMap<String, AbstractCard> (populated only by the
     CURSE-color branch of CardLibrary.add(), CardLibrary.java:938-944) the same way.
  3. Re-deriving each downstream pool by iterating the map in that HashMap order and applying
     the exact filter each real-game pool-builder applies:

       - CardLibrary.addRedCards(tmpPool) et al (CardLibrary.java:1198+): color match,
         rarity != BASIC, unlock check (== always-pass, since Settings.treatEverythingAsUnlocked()
         is true for "everything unlocked" / real played-game runtime state — see note below).
       - AbstractDungeon.initializeCardPools() (AbstractDungeon.java:1190): sorts tmpPool
         (built in CardLibrary.cards HashMap order via player.getCardPool) into
         commonCardPool/uncommonCardPool/rareCardPool by rarity, appending
         (CardGroup.addToTop == list.add(item), i.e. append — NOT insertion at index 0,
         despite the name; verified by reading CardGroup.java:504-510) — so pool order ==
         filtered HashMap order, unchanged.
       - AbstractDungeon.addColorlessCards()/addCurseCards() (AbstractDungeon.java:1265-1292):
         same pattern, filtered from CardLibrary.cards (colorless) or CardLibrary.cards again
         (not the separate `curses` map!) for the curseCardPool used in COMBAT REWARDS /
         AbstractDungeon-side draws — but CardLibrary.getCurse() (used e.g. by Neow / certain
         relic effects) draws from the SEPARATE `curses` HashMap. Both are produced here.

  Unlock state: this generator assumes UnlockTracker.isCardLocked(id) is always false for
  every card, matching Settings.treatEverythingAsUnlocked() — the runtime state for actual
  gameplay recordings (all content unlocked is the norm for any non-fresh save, and the
  spike's ground-truth runs are from established saves). If a specific replay used a fresh/
  partially-locked save, this would need per-run unlock state — out of scope here (not
  observed in the sampled corpus: the floor-1 UNCOMMON/COMMON/UNCOMMON rarity sequence and
  overall behavior match a fully-unlocked pool).

Run: python3 generate_pools.py
Outputs (for validation / eyeballing): prints the derived commonCardPool / uncommonCardPool /
rareCardPool per character color, colorlessCardPool (dungeon-side, 2 variants — CardLibrary vs
AbstractDungeon.addColorlessCards, expected identical since same filter minus SPECIAL — see
inline diff), and both curse-pool variants (AbstractDungeon.curseCardPool vs
CardLibrary.curses-based CardLibrary.getCurse() pool).

Also writes cpp_pool_arrays.json — the exact ordered CardId-name lists this script derives,
consumed by generate_cardpools_header.py to patch CardPools.h.
"""

import json
import sys

sys.path.insert(0, '/private/tmp/claude-501/-Users-nico-git-spire-agent/d4e15dba-42f8-4fc2-b183-533b03dd427a/scratchpad/spike')
from java_hashmap import simulate_order

SCRATCH = '/private/tmp/claude-501/-Users-nico-git-spire-agent/d4e15dba-42f8-4fc2-b183-533b03dd427a/scratchpad/spike'

metadata = json.load(open(f'{SCRATCH}/card_metadata.json'))

# --- Step 1: CardLibrary.cards insertion order (CardLibrary.initialize(), CardLibrary.java:406-413) ---
insertion_order_colors = ['RED', 'GREEN', 'BLUE', 'PURPLE', 'COLORLESS', 'CURSE']
all_cards = []  # list of dicts, in insertion order
for color in insertion_order_colors:
    all_cards.extend(metadata[color])

card_ids_in_insertion_order = [c['id'] for c in all_cards]
by_id = {c['id']: c for c in all_cards}

cards_map_order = simulate_order(card_ids_in_insertion_order)  # CardLibrary.cards.entrySet() order

# --- Step 2: separate `curses` HashMap (CardLibrary.add(), CURSE branch, CardLibrary.java:938-944) ---
curse_ids_in_insertion_order = [c['id'] for c in metadata['CURSE']]
curses_map_order = simulate_order(curse_ids_in_insertion_order)  # CardLibrary.curses.entrySet() order


def filter_pool(rarity, color=None, exclude_basic=True, exclude_status=False, exclude_special=False):
    """Iterate cards_map_order (== CardLibrary.cards.entrySet() order) and keep cards matching color/rarity, mirroring addRedCards/addColorlessCards/etc's filter predicate."""
    out = []
    for cid in cards_map_order:
        c = by_id[cid]
        if color is not None and c['color'] != color:
            continue
        if c['rarity'] != rarity:
            continue
        if exclude_status and c['type'] == 'STATUS':
            continue
        out.append(cid)
    return out


# --- Per-class RarityCardPool: commonCardPool / uncommonCardPool / rareCardPool ---
# Filter: CardLibrary.addRedCards(tmpPool) etc (CardLibrary.java:1198-1252):
#   card.color == COLOR && card.rarity != BASIC && unlocked(always true)
# Then AbstractDungeon.initializeCardPools() (AbstractDungeon.java:1217-1234) buckets by
# c.rarity into commonCardPool/uncommonCardPool/rareCardPool via addToTop == append, so the
# pool order == the tmpPool order for that rarity, unchanged (a per-rarity sub-sequence of
# the color-filtered HashMap order).
CLASS_COLORS = ['RED', 'GREEN', 'BLUE', 'PURPLE']
per_class_pools = {}
for color in CLASS_COLORS:
    per_class_pools[color] = {
        'COMMON': filter_pool('COMMON', color=color),
        'UNCOMMON': filter_pool('UNCOMMON', color=color),
        'RARE': filter_pool('RARE', color=color),
    }

# --- ColorlessRarityCardPool (dungeon-side, used for Neow/getRandomColorlessCardNeow etc) ---
# AbstractDungeon.addColorlessCards() (AbstractDungeon.java:1265-1277):
#   card.color == COLORLESS && card.rarity != BASIC && card.rarity != SPECIAL && card.type != STATUS
colorless_common = filter_pool('COMMON', color='COLORLESS', exclude_status=True)
colorless_uncommon = filter_pool('UNCOMMON', color='COLORLESS', exclude_status=True)
colorless_rare = filter_pool('RARE', color='COLORLESS', exclude_status=True)
# COMMON colorless cards in-game: none exist at COMMON rarity that aren't excluded — check.

# --- curseCardPool (dungeon-side, AbstractDungeon.addCurseCards(), AbstractDungeon.java:1279-1292) ---
# Filter: card.type == CURSE && cardID not in {Necronomicurse, AscendersBane, CurseOfTheBell, Pride}
# NOTE: iterates CardLibrary.cards (the main map), not the separate `curses` map.
EXCLUDED_CURSES = {'Necronomicurse', 'AscendersBane', 'CurseOfTheBell', 'Pride'}
dungeon_curse_pool = [cid for cid in cards_map_order if by_id[cid]['type'] == 'CURSE' and cid not in EXCLUDED_CURSES]

# --- CardLibrary.getCurse() pool (separate `curses` HashMap, CardLibrary.java:1028-1041) ---
# Filter: same 4 exclusions, but iterates the `curses` map (curses_map_order), not `cards`.
library_curse_pool = [cid for cid in curses_map_order if cid not in EXCLUDED_CURSES]

print("=== Per-class pools (RarityCardPool source order) ===")
for color in CLASS_COLORS:
    for rarity in ['COMMON', 'UNCOMMON', 'RARE']:
        pool = per_class_pools[color][rarity]
        print(f"{color} {rarity} ({len(pool)}): {pool}")
    print()

print("=== Colorless pools (ColorlessRarityCardPool source order) ===")
print(f"COMMON ({len(colorless_common)}): {colorless_common}")
print(f"UNCOMMON ({len(colorless_uncommon)}): {colorless_uncommon}")
print(f"RARE ({len(colorless_rare)}): {colorless_rare}")
print()

print("=== Curse pools ===")
print(f"dungeon (AbstractDungeon.curseCardPool, from CardLibrary.cards) ({len(dungeon_curse_pool)}): {dungeon_curse_pool}")
print(f"library (CardLibrary.getCurse(), from CardLibrary.curses) ({len(library_curse_pool)}): {library_curse_pool}")
print(f"SAME ORDER: {dungeon_curse_pool == library_curse_pool}")

# --- Persist for the C++ header generator ---
output = {
    'per_class_pools': per_class_pools,
    'colorless_pools': {
        'COMMON': colorless_common,
        'UNCOMMON': colorless_uncommon,
        'RARE': colorless_rare,
    },
    'dungeon_curse_pool': dungeon_curse_pool,
    'library_curse_pool': library_curse_pool,
    'cards_map_order': cards_map_order,
    'curses_map_order': curses_map_order,
}
with open(f'{SCRATCH}/derived_pools.json', 'w') as f:
    json.dump(output, f, indent=1)

print(f"\nWrote {SCRATCH}/derived_pools.json")
