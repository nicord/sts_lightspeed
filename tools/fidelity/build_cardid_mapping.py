"""
Builds the Java cardID-string -> sts_lightspeed CardId enum mapping for every card in
CardLibrary (370 entries), and writes java_to_cppcardid_mapping.json.

Most Java cardID strings equal the card's current display name (mod whitespace/punctuation
normalized to SNAKE_CASE), which is exactly how sts_lightspeed's CardId enum names were
derived, so a normalize()+lookup resolves ~92% automatically (341/370).

The remaining 29 are cards whose Java `cardID` field is a STALE INTERNAL NAME left over from
a past game version's balance patch that renamed the card (Slay the Spire has changed several
card names post-release, e.g. "Underhanded Strike" -> "Sneaky Strike", "Venomology" ->
"Alchemize" -- the display name (and the card's `NAME` in localization/eng/cards.json) updated,
but `cardID`/the HashMap key did NOT, since AbstractCard.cardID is also the save-file/deck
serialization key and changing it would break existing saves). sts_lightspeed's CardId enum,
by contrast, was authored against CURRENT display names, so these need an explicit override.

Verified by cross-referencing each stale ID against target/classes/localization/eng/cards.json's
NAME field in the real spire-src checkout's compiled resources (confirms the current in-game
display name for that Java-internal ID), then normalizing THAT display name to find the CardId
enum entry.

The three Defend_R/Strike_R-style BASIC-rarity starter cards (Defend_Red, Strike_Red, and their
green/blue/purple counterparts) are intentionally left unmapped -- they're CardRarity.BASIC,
which every real-game pool-builder filter (addRedCards/addColorlessCards/etc, CardLibrary.java)
explicitly excludes (`card.rarity != AbstractCard.CardRarity.BASIC`), so they never appear in
any of the pools this fix touches. Mapping them is not needed for correctness here, but they ARE
still counted in the CardLibrary.cards HashMap population (they occupy buckets / affect resize
timing) -- java_hashmap.py's simulation already includes them via their Java cardID string
(e.g. "Strike_R"), it just never needs to translate that string to a CardId enum value since
they're filtered out downstream.
"""
import json
import re

SCRATCH = '/private/tmp/claude-501/-Users-nico-git-spire-agent/d4e15dba-42f8-4fc2-b183-533b03dd427a/scratchpad/spike'

with open(f'{SCRATCH}/cpp_cardid_names.txt') as f:
    cpp_names = set(l.strip() for l in f if l.strip())

with open(f'{SCRATCH}/card_metadata.json') as f:
    metadata = json.load(f)


def normalize(java_id):
    s = re.sub(r"[^A-Za-z0-9]+", "_", java_id)
    s = re.sub(r'(?<=[a-z0-9])(?=[A-Z])', '_', s)
    s = s.upper()
    s = re.sub(r'_+', '_', s).strip('_')
    return s


# Explicit overrides for stale/renamed internal Java cardID strings, each verified against
# target/classes/localization/eng/cards.json's NAME field for that ID (the CURRENT display
# name), then matched to the CardId enum entry authored from that current name.
RENAME_OVERRIDES = {
    'Underhanded Strike': 'SNEAKY_STRIKE',     # -> "Sneaky Strike"
    'Venomology': 'ALCHEMIZE',                 # -> "Alchemize"
    'Crippling Poison': 'CRIPPLING_CLOUD',     # -> "Crippling Cloud"
    'Night Terror': 'NIGHTMARE',               # -> "Nightmare"
    'Wraith Form v2': 'WRAITH_FORM',           # -> "Wraith Form"
    'Gash': 'CLAW',                            # -> "Claw"
    'Lockon': 'BULLSEYE',                      # -> "Bullseye"
    'Steam Power': 'OVERCLOCK',                # -> "Overclock"
    'Redo': 'RECURSION',                       # -> "Recursion"
    'Steam': 'STEAM_BARRIER',                  # -> "Steam Barrier"
    'Undo': 'EQUILIBRIUM',                     # -> "Equilibrium"
    'Fasting2': 'FASTING',                     # -> "Fasting"
    'Wireheading': 'FORESIGHT',                # -> "Foresight"
    'Judgement': 'JUDGMENT',                   # -> "Judgment" (British->American spelling fix)
    'PathToVictory': 'PRESSURE_POINTS',        # -> "Pressure Points"
    'Adaptation': 'RUSHDOWN',                  # -> "Rushdown"
    'Vengeance': 'SIMMERING_FURY',             # -> "Simmering Fury"
    'ClearTheMind': 'TRANQUILITY',             # -> "Tranquility"
    'Ghostly': 'APPARITION',                   # -> "Apparition"
    'J.A.X.': 'JAX',                           # punctuation stripped in the enum, not a rename
    'Conserve Battery': 'CHARGE_BATTERY',      # -> "Charge Battery"
}

# BASIC-rarity starter cards: excluded from every real pool by CardLibrary's own filters
# (rarity != BASIC), so a CardId mapping isn't required for pool generation, but recorded here
# for completeness/documentation.
BASIC_UNMAPPED = {'Defend_R', 'Strike_R', 'Defend_G', 'Strike_G', 'Defend_B', 'Strike_B', 'Defend_P', 'Strike_P'}

all_cards = []
for color in ['RED', 'GREEN', 'BLUE', 'PURPLE', 'COLORLESS', 'CURSE']:
    all_cards.extend(metadata[color])

mapping = {}
unmatched = []
for c in all_cards:
    jid = c['id']
    if jid in RENAME_OVERRIDES:
        mapping[jid] = RENAME_OVERRIDES[jid]
        continue
    if jid in BASIC_UNMAPPED:
        continue  # deliberately excluded; not needed downstream
    norm = normalize(jid)
    if norm in cpp_names:
        mapping[jid] = norm
    else:
        unmatched.append((jid, norm, c['class']))

print(f"total cards: {len(all_cards)}, mapped: {len(mapping)}, deliberately unmapped (BASIC): {len(BASIC_UNMAPPED)}, unresolved: {len(unmatched)}")
for u in unmatched:
    print("UNRESOLVED:", u)

with open(f'{SCRATCH}/java_to_cppcardid_mapping.json', 'w') as f:
    json.dump(mapping, f, indent=1, sort_keys=True)

print(f"wrote {SCRATCH}/java_to_cppcardid_mapping.json")
