"""
Root-cause fix: the recorded ground-truth corpus (archive/logs-2a-cardprior-ab-970runs-...)
was generated in Docker (docker/entrypoint.sh), which never creates a `STSUnlocks` preferences
file. Real game boot logic (UnlockTracker.refresh(), UnlockTracker.java:198-266) hardcodes 9
"beat the boss enough times" unlockable cards per character color (3 COMMON + 3 UNCOMMON + 3 RARE),
gated on `unlockPref.getInteger(key, 0) == 2`. With no prefs file, every lookup defaults to 0,
so ALL 36 of these cards (9 per RED/GREEN/BLUE/PURPLE) are LOCKED for every Docker-recorded run.

AbstractDungeon.initializeCardPools() -> CardLibrary.addRedCards(tmpPool) etc (CardLibrary.java:
1198-1252) filter each color's card list by `!UnlockTracker.isCardLocked(id) ||
Settings.treatEverythingAsUnlocked()` (treatEverythingAsUnlocked() is false for a normal seeded
CommunicationMod `start` run -- only true for isDailyRun/isTrial). So commonCardPool/
uncommonCardPool/rareCardPool (built from this filtered list, in HashMap order) are missing
these locked cards -- shrinking pool sizes and shifting every index a cardRng draw resolves to.

Verified byte-for-byte against 2 independent seeds by relaunching the actual game headlessly
(sts/run.sh) with a scripted CommunicationMod driver and replaying the exact seed/ascension/
Neow-choice/path: both floor-1 card rewards match ONLY when RarityCardPool excludes these 36
cards; the currently-committed (fully-unlocked, size 20/36/16 for Red) pool gives the wrong cards
even though the RNG stream position is provably identical to the real game (confirmed via the
game's own com.megacrit.cardcrawl.random.Random class run standalone against the same seed).

This script regenerates RarityCardPool's cardBlob/groupOffset/groupSize (and the analogous
Colorless/curse pools, which have NO lockable entries so are unaffected) with the 36 Docker-locked
cards excluded, and patches CardPools.h.
"""
import json
import re
import sys

sys.path.insert(0, '/private/tmp/claude-501/-Users-nico-git-spire-agent/d4e15dba-42f8-4fc2-b183-533b03dd427a/scratchpad/spike')
from java_hashmap import simulate_order

SCRATCH = '/private/tmp/claude-501/-Users-nico-git-spire-agent/d4e15dba-42f8-4fc2-b183-533b03dd427a/scratchpad/spike'
HEADER_PATH = '/Users/nico/git/sts_lightspeed/include/constants/CardPools.h'

# The exact 36 cards UnlockTracker.refresh() (UnlockTracker.java:203-238) gates behind
# unlockPref -- 9 per RED/GREEN/BLUE/PURPLE (3 COMMON + 3 UNCOMMON + 3 RARE each). Confirmed
# against both the decompiled source and the installed jar's live STSUnlocks prefs file
# (this exact key set appears in ~/Library/.../preferences/STSUnlocks with value "2" wherever
# it has actually been unlocked on a real save).
DOCKER_LOCKED_CARDS = {
    # RED
    "Havoc", "Sentinel", "Exhume", "Wild Strike", "Evolve", "Immolate",
    "Heavy Blade", "Spot Weakness", "Limit Break",
    # GREEN
    "Concentrate", "Setup", "Grand Finale", "Cloak And Dagger", "Accuracy",
    "Storm of Steel", "Bane", "Catalyst", "Corpse Explosion",
    # BLUE
    "Rebound", "Undo", "Echo Form", "Turbo", "Sunder", "Meteor Strike",
    "Hyperbeam", "Recycle", "Core Surge",
    # PURPLE
    "Prostrate", "Blasphemy", "Devotion", "ForeignInfluence", "Alpha",
    "MentalFortress", "SpiritShield", "Wish", "Wireheading",
}
assert len(DOCKER_LOCKED_CARDS) == 36

metadata = json.load(open(f'{SCRATCH}/card_metadata.json'))

insertion_order_colors = ['RED', 'GREEN', 'BLUE', 'PURPLE', 'COLORLESS', 'CURSE']
all_cards = []
for color in insertion_order_colors:
    all_cards.extend(metadata[color])
card_ids_in_insertion_order = [c['id'] for c in all_cards]
by_id = {c['id']: c for c in all_cards}

# CardLibrary.cards' HashMap order is UNAFFECTED by lock state (CardLibrary.add() puts every
# card unconditionally -- the lock check only happens in the addRedCards/etc FILTER that reads
# from this already-fully-populated map). So the base map order is the same as before; only the
# per-color/per-rarity filtered pools change.
cards_map_order = simulate_order(card_ids_in_insertion_order)


def filter_pool(rarity, color):
    out = []
    for cid in cards_map_order:
        c = by_id[cid]
        if c['color'] != color or c['rarity'] != rarity:
            continue
        if cid in DOCKER_LOCKED_CARDS:
            continue
        out.append(cid)
    return out


CLASS_COLORS = ['RED', 'GREEN', 'BLUE', 'PURPLE']
RARITIES = ['COMMON', 'UNCOMMON', 'RARE']
per_class_pools = {}
for color in CLASS_COLORS:
    per_class_pools[color] = {r: filter_pool(r, color) for r in RARITIES}
    for r in RARITIES:
        print(f"{color} {r}: {len(per_class_pools[color][r])} {per_class_pools[color][r]}")

# --- Build cardBlob (flat, color-major then rarity-major, matching the existing layout) + new
# groupOffset/groupSize tables (sizes now shrink per the exclusions above). ---
flat = []
offsets = []  # [color][rarity]
sizes = []    # [color][rarity]
cursor = 0
for color in CLASS_COLORS:
    color_offsets = []
    color_sizes = []
    for r in RARITIES:
        pool = per_class_pools[color][r]
        color_offsets.append(cursor)
        color_sizes.append(len(pool))
        flat.extend(pool)
        cursor += len(pool)
    offsets.append(color_offsets)
    sizes.append(color_sizes)

CARDID_UPPER = {
    # Only need entries actually differing from a straightforward "insert underscores before
    # capitals, uppercase" transform -- but simplest/most-robust is to reuse the existing
    # CardId enum names already present in the header for these same card strings by cross-
    # referencing java_to_cppcardid_mapping.json (built during the original pool-order fix).
}
mapping = json.load(open(f'{SCRATCH}/java_to_cppcardid_mapping.json'))


def to_cardid(java_id):
    if java_id not in mapping:
        raise KeyError(f"no CardId mapping for {java_id!r}")
    return mapping[java_id]


def cardid_list(java_ids):
    return ','.join(f'CardId::{to_cardid(j)}' for j in java_ids)


new_cardblob = f"        static constexpr CardId cardBlob[] {{{cardid_list(flat)},}};"
offset_str = ','.join('{' + ','.join(str(x) for x in row) + ',}' for row in offsets)
size_str = ','.join('{' + ','.join(str(x) for x in row) + ',}' for row in sizes)
new_group_offset = f"        static constexpr int groupOffset[4][3] {{{offset_str},}};"
new_group_size = f"        static constexpr int groupSize[4][3] {{{size_str},}};"

with open(HEADER_PATH) as f:
    content = f.read()

# --- locate and replace the existing cardBlob / groupOffset / groupSize lines inside
# `namespace RarityCardPool { ... }` by regex on their distinctive prefixes (single-line
# declarations in the current header). ---
def replace_line(content, prefix, new_line):
    pattern = re.compile(re.escape(prefix) + r'[^\n]*\n')
    m = pattern.search(content)
    if not m:
        raise RuntimeError(f"pattern not found for prefix: {prefix[:60]}")
    return content[:m.start()] + new_line + "\n" + content[m.end():]


content = replace_line(content, "        static constexpr CardId cardBlob[]", new_cardblob)
content = replace_line(content, "        static constexpr int groupOffset[4][3]", new_group_offset)
content = replace_line(content, "        static constexpr int groupSize[4][3]", new_group_size)

with open(HEADER_PATH, 'w') as f:
    f.write(content)

print("\nPatched", HEADER_PATH)
print("New groupOffset:", offsets)
print("New groupSize:", sizes)
