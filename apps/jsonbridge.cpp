// jsonbridge: a stdio JSON bridge that drives one full Slay the Spire run
// (GameContext) and, at each OUT-OF-COMBAT decision point, prints ONE JSON
// line to stdout describing the screen + choices, then reads one JSON line
// from stdin `{"choose": <index>}` to pick.
//
// Combat is NOT bridged: it is resolved internally by a battle agent chosen
// via `--combat=simple|scum` (default simple):
//  - simple: sts::search::SimpleAgent, the cheapest competent existing battle
//    AI in this codebase (a fast greedy heuristic).
//  - scum: sts::search::ScumSearchAgent2's playoutBattle(), which drives the
//    same MCTS BattleScumSearcher2 used by apps/main.cpp's scum-search modes
//    (see ScumSearchAgent2.cpp) -- a much stronger, much slower battle AI.
//    The simulation count per decision is controlled by `--combat-sims=N`
//    (mirrors ScumSearchAgent2::simulationCountBase; default 1000). Each
//    BattleScumSearcher2 is constructed fresh per decision point and seeds
//    its own RNG deterministically from `bc.seed + bc.floorNum` (see
//    BattleScumSearcher2's constructor), so search(N) always runs exactly N
//    simulations and returns -- no wall-clock dependency, so results stay
//    reproducible per run seed.
//    CAVEAT: BattleScumSearcher2's random rollout (playoutRandom) has no
//    internal step bound, so on rare pathological battle states (observed on
//    real seeds) a single step() can hang regardless of --combat-sims. See
//    tryPlayoutBattleScum()/resolveBattle(): every --combat=scum battle runs
//    under a `--combat-timeout-ms` wall-clock watchdog (default 8000ms) on a
//    worker thread and falls back to SimpleAgent for that one fight if the
//    deadline passes, so the run always terminates.
//
// Reference material used while writing this (do not delete, useful context
// for future edits):
//  - src/sim/ConsoleSimulator.cpp: exact GameContext call patterns per screen.
//  - src/sim/search/SimpleAgent.cpp (::playout / ::playoutBattle): the
//    BattleContext init/playoutBattle/exitBattle recipe.
//  - src/sim/search/ScumSearchAgent2.cpp (::playoutBattle): the
//    BattleScumSearcher2 construct/search(simulationCount)/step recipe used
//    for --combat=scum.
//  - the prior fidelity_harness.cpp spike: confirmed method names/signatures
//    for chooseNeowOption/transitionToMapNode/chooseSelectCardScreenOption/
//    chooseEventOption/chooseCampfireOption/chooseTreasureRoomOption/
//    chooseBossRelic.
//
// This binary is driven by an external TypeScript process over piped
// stdin/stdout to run many full-game simulations for calibration experiments.

#include <iostream>
#include <sstream>
#include <string>
#include <vector>
#include <cstdint>
#include <random>
#include <thread>
#include <chrono>
#include <atomic>

#include <nlohmann/json.hpp>

#include "game/Game.h"
#include "game/GameContext.h"
#include "game/Map.h"
#include "game/Neow.h"
#include "game/Card.h"
#include "game/Deck.h"
#include "game/RelicContainer.h"
#include "game/Shop.h"
#include "constants/Cards.h"
#include "constants/Potions.h"
#include "constants/Relics.h"
#include "constants/Rooms.h"
#include "constants/Events.h"
#include "constants/Misc.h"

#include "combat/BattleContext.h"
#include "sim/search/SimpleAgent.h"
#include "sim/search/ScumSearchAgent2.h"
#include "sim/search/BattleScumSearcher2.h"

using namespace sts;
using json = nlohmann::json;

namespace {

bool g_auto = false;

/** Which battle agent resolves combat: SIMPLE (SimpleAgent, default) or SCUM (ScumSearchAgent2 MCTS). */
enum class CombatMode { SIMPLE, SCUM };
CombatMode g_combatMode = CombatMode::SIMPLE;

/** Simulations per decision point for --combat=scum (mirrors ScumSearchAgent2::simulationCountBase). */
std::int64_t g_combatSims = 1000;

/**
 * Wall-clock cap (ms) for a single --combat=scum battle. BattleScumSearcher2's random rollout
 * (playoutRandom) has no internal step bound, so a battle state that never terminates under pure
 * random play (rare, but observed on real seeds -- e.g. certain block/regen stalemates) can hang a
 * `step()` call indefinitely regardless of --combat-sims. This is a safety net, not a tuning knob.
 */
std::int64_t g_combatTimeoutMs = 8000;

/** Reads one `{"choose": <index>}` line from stdin (blocking), or returns a programmatic default in --auto mode. */
int readChoice(int autoDefault) {
    if (g_auto) {
        return autoDefault;
    }
    std::string line;
    if (!std::getline(std::cin, line)) {
        // stdin closed unexpectedly; treat as "pick default" so we terminate cleanly.
        return autoDefault;
    }
    try {
        auto j = json::parse(line);
        return j.at("choose").get<int>();
    } catch (const std::exception &e) {
        std::cerr << "jsonbridge: failed to parse choose line \"" << line << "\": " << e.what() << std::endl;
        return autoDefault;
    }
}

/** Serializes a Card to the JSON shape used in `deck`/choice card fields (id/name/type/rarity/cost/upgraded). */
json cardToJson(const Card &c) {
    json j;
    j["id"] = cardEnumStrings[static_cast<int>(c.getId())];
    j["name"] = c.getName();
    switch (c.getType()) {
        case CardType::ATTACK: j["type"] = "ATTACK"; break;
        case CardType::SKILL: j["type"] = "SKILL"; break;
        case CardType::POWER: j["type"] = "POWER"; break;
        case CardType::CURSE: j["type"] = "CURSE"; break;
        case CardType::STATUS: j["type"] = "STATUS"; break;
        default: j["type"] = "INVALID"; break;
    }
    switch (c.getRarity()) {
        case CardRarity::COMMON: j["rarity"] = "COMMON"; break;
        case CardRarity::UNCOMMON: j["rarity"] = "UNCOMMON"; break;
        case CardRarity::RARE: j["rarity"] = "RARE"; break;
        case CardRarity::BASIC: j["rarity"] = "BASIC"; break;
        case CardRarity::SPECIAL: j["rarity"] = "SPECIAL"; break;
        case CardRarity::CURSE: j["rarity"] = "CURSE"; break;
        default: j["rarity"] = "INVALID"; break;
    }
    j["cost"] = getEnergyCost(c.getId(), c.isUpgraded());
    j["upgraded"] = c.isUpgraded();
    return j;
}

/** Builds the `state` object shared by every non-GAME_OVER screen line (floor/hp/gold/deck/relics/potions). */
json buildStateJson(const GameContext &gc) {
    json state;
    state["floor"] = gc.floorNum;
    state["act"] = gc.act;
    state["hp"] = gc.curHp;
    state["maxHp"] = gc.maxHp;
    state["gold"] = gc.gold;
    // The current act's boss encounter name (e.g. "Slime Boss"), known from the act's start. The TS
    // handlers read it as `game_state.act_boss` for `actBoss` KB conditions (card conditional rules,
    // event rules) and boss-relic/map context. INVALID before a boss is assigned emits "INVALID",
    // which no rule matches (fail-closed).
    state["actBoss"] = monsterEncounterStrings[static_cast<int>(gc.boss)];

    json deck = json::array();
    for (const auto &c : gc.deck.cards) {
        deck.push_back(cardToJson(c));
    }
    state["deck"] = deck;

    json relics = json::array();
    for (const auto &r : gc.relics.relics) {
        json rj;
        rj["id"] = getRelicName(r.id);
        rj["name"] = getRelicName(r.id);
        // Per-relic counter (RelicInstance.data): charges/tallies the engine maintains, e.g. Neow's
        // Lament remaining combats, Velvet Choker cards-played, Ink Bottle/Nunchaku/Happy Flower. The
        // agent handlers read relic counters (e.g. buff-aware routing keys on Neow's Lament); without
        // this they'd all see 0.
        rj["counter"] = r.data;
        relics.push_back(rj);
    }
    state["relics"] = relics;

    json potions = json::array();
    for (int i = 0; i < gc.potionCapacity; ++i) { // slots can be sparse after drinking
        if (gc.potions[i] != Potion::EMPTY_POTION_SLOT && gc.potions[i] != Potion::INVALID) {
            potions.push_back(potionEnumNames[static_cast<int>(gc.potions[i])]);
        }
    }
    state["potions"] = potions;

    return state;
}

/** Prints one screen-decision JSON line (screen name + state + choices) and flushes stdout. */
void emitLine(const std::string &screen, const json &state, const json &choices) {
    json out;
    out["screen"] = screen;
    out["state"] = state;
    out["choices"] = choices;
    std::cout << out.dump() << std::endl; // std::endl flushes
}

/** Prints the terminal GAME_OVER line and exits the process. */
[[noreturn]] void emitGameOverAndExit(const GameContext &gc) {
    json out;
    out["screen"] = "GAME_OVER";
    out["victory"] = (gc.outcome == GameOutcome::PLAYER_VICTORY);
    out["floor"] = gc.floorNum;
    out["act"] = gc.act;
    // Score proxy: gold at run end. Simple, always available, monotonic enough
    // for a coarse calibration signal; the driver is free to compute richer
    // scores itself from the state lines it already collected along the way.
    out["score"] = gc.gold;
    std::cout << out.dump() << std::endl;
    std::exit(0);
}

// ---------------------------------------------------------------------------
// MAP_SCREEN
// ---------------------------------------------------------------------------

/** Serializes the whole current-act map as [{x,y,symbol,children:[{x,y}]}] for driver-side lookahead. */
json mapToJson(const GameContext &gc) {
    json nodes = json::array();
    for (int y = 0; y < 15; ++y) {
        for (int x = 0; x < 7; ++x) {
            const MapNode &n = gc.map->getNode(x, y);
            if (n.room == Room::NONE || n.room == Room::INVALID) continue;
            json nj;
            nj["x"] = x;
            nj["y"] = y;
            nj["symbol"] = std::string(1, getRoomSymbol(n.room));
            json children = json::array();
            for (int i = 0; i < n.edgeCount; ++i) {
                children.push_back(json{{"x", n.edges[i]}, {"y", y + 1}});
            }
            nj["children"] = children;
            nodes.push_back(nj);
        }
    }
    return nodes;
}

/** Handles MAP_SCREEN: emits available next-node choices, reads back the chosen map-node x. */
void handleMapScreen(GameContext &gc) {
    json choices = json::array();

    if (gc.curMapNodeY == 14) {
        json c;
        c["index"] = 0;
        c["label"] = "Advance to Boss";
        c["symbol"] = "B";
        choices.push_back(c);
    } else if (gc.curMapNodeY == -1) {
        for (const auto &firstRowNode : gc.map->nodes[0]) {
            if (firstRowNode.edgeCount > 0) {
                json c;
                c["index"] = firstRowNode.x;
                c["x"] = firstRowNode.x;
                c["y"] = firstRowNode.y;
                c["symbol"] = std::string(1, getRoomSymbol(firstRowNode.room));
                c["label"] = roomStrings[static_cast<int>(firstRowNode.room)];
                choices.push_back(c);
            }
        }
    } else {
        const auto &node = gc.map->getNode(gc.curMapNodeX, gc.curMapNodeY);
        for (int i = 0; i < node.edgeCount; ++i) {
            const auto nextNodeX = node.edges[i];
            const auto &nextNode = gc.map->getNode(nextNodeX, node.y + 1);
            json c;
            c["index"] = nextNode.x;
            c["x"] = nextNode.x;
            c["y"] = nextNode.y;
            c["symbol"] = std::string(1, getRoomSymbol(nextNode.room));
            c["label"] = roomStrings[static_cast<int>(nextNode.room)];
            choices.push_back(c);
        }
    }

    json state = buildStateJson(gc);
    state["map"] = mapToJson(gc); // full act map for driver-side route lookahead
    emitLine("MAP", state, choices);

    int defaultIdx = choices.empty() ? 0 : choices[0]["index"].get<int>();
    int choice = readChoice(defaultIdx);
    gc.transitionToMapNode(choice);
}

// ---------------------------------------------------------------------------
// REWARDS
// ---------------------------------------------------------------------------

/**
 * Handles REWARDS: auto-takes gold/relic/potion/keys (always beneficial, no
 * real decision), and only surfaces a genuine choice for CARD rewards (or a
 * skip/proceed). Loops internally on non-card auto-takes without emitting a
 * line, per the wire-format spec (only emit when there's a real decision).
 */
void handleRewards(GameContext &gc) {
    auto &r = gc.info.rewardsContainer;

    // Auto-take everything except card rewards.
    while (true) {
        if (r.goldRewardCount > 0) {
            gc.obtainGold(r.gold[0]);
            r.removeGoldReward(0);
            continue;
        }
        if (r.relicCount > 0) {
            bool loseSapphireKeyRelic = r.sapphireKey && (0 == r.relicCount - 1);
            gc.obtainRelic(r.relics[0]);
            if (loseSapphireKeyRelic) {
                r.sapphireKey = false;
            }
            r.removeRelicReward(0);
            continue;
        }
        if (r.potionCount > 0) {
            gc.obtainPotion(r.potions[0]);
            r.removePotionReward(0);
            continue;
        }
        if (r.emeraldKey) {
            gc.obtainKey(Key::EMERALD_KEY);
            r.emeraldKey = false;
            continue;
        }
        if (r.sapphireKey) {
            if (r.relicCount > 0) {
                r.removeRelicReward(r.relicCount - 1);
            }
            gc.obtainKey(Key::SAPPHIRE_KEY);
            r.sapphireKey = false;
            continue;
        }
        break;
    }

    if (r.cardRewardCount == 0) {
        gc.regainControl();
        return;
    }

    // Present card reward choices: one per card in the (first) card reward slot,
    // plus a skip. (In practice cardRewardCount is 1 outside of a couple of
    // relic-driven edge cases; we handle slot 0, which covers the standard
    // "pick one of up to 4 cards" reward screen.)
    json choices = json::array();
    const auto &reward = r.cardRewards[0];
    for (int ci = 0; ci < reward.size(); ++ci) {
        json c = cardToJson(reward[ci]);
        c["index"] = ci;
        c["label"] = std::string("Take ") + reward[ci].getName();
        choices.push_back(c);
    }
    json skip;
    skip["index"] = static_cast<int>(reward.size());
    skip["label"] = "Skip card reward";
    choices.push_back(skip);

    emitLine("CARD_REWARD", buildStateJson(gc), choices);

    int choice = readChoice(0); // --auto default: take card 0
    if (choice >= 0 && choice < reward.size()) {
        gc.deck.obtain(gc, reward[choice]);
        r.removeCardReward(0);
    } else {
        r.removeCardReward(0);
    }

    if (r.getTotalCount() == 0) {
        gc.regainControl();
    }
}

// ---------------------------------------------------------------------------
// BOSS_RELIC_REWARDS
// ---------------------------------------------------------------------------

/** Handles BOSS_RELIC_REWARDS: three relic choices + skip (index 3), mirroring ConsoleSimulator's numbering. */
void handleBossRelicRewards(GameContext &gc) {
    json choices = json::array();
    for (int i = 0; i < 3; ++i) {
        json c;
        c["index"] = i;
        c["label"] = getRelicName(gc.info.bossRelics[i]);
        choices.push_back(c);
    }
    json skip;
    skip["index"] = 3;
    skip["label"] = "Skip Reward";
    choices.push_back(skip);

    emitLine("BOSS_RELIC", buildStateJson(gc), choices);

    int choice = readChoice(0);
    gc.chooseBossRelic(choice);
}

// ---------------------------------------------------------------------------
// CARD_SELECT (Smith/Purge/Transform/Bottle/Bonfire Spirits, etc.)
// ---------------------------------------------------------------------------

/** Human name for a CardSelectScreenType — tells the driver WHY it is picking a card. */
const char *selectTypeName(CardSelectScreenType t) {
    switch (t) {
        case CardSelectScreenType::TRANSFORM: return "TRANSFORM";
        case CardSelectScreenType::TRANSFORM_UPGRADE: return "TRANSFORM_UPGRADE";
        case CardSelectScreenType::UPGRADE: return "UPGRADE";
        case CardSelectScreenType::REMOVE: return "REMOVE";
        case CardSelectScreenType::DUPLICATE: return "DUPLICATE";
        case CardSelectScreenType::OBTAIN: return "OBTAIN";
        case CardSelectScreenType::BOTTLE: return "BOTTLE";
        case CardSelectScreenType::BONFIRE_SPIRITS: return "BONFIRE_SPIRITS";
        default: return "INVALID";
    }
}

/** Handles CARD_SELECT: post-reward-adjacent card picks (rest-site Smith, event Remove/Transform/Upgrade/Bottle/Bonfire). */
void handleCardSelect(GameContext &gc) {
    json choices = json::array();
    for (int i = 0; i < gc.info.toSelectCards.size(); ++i) {
        json c = cardToJson(gc.info.toSelectCards[i].card);
        c["index"] = i;
        c["label"] = gc.info.toSelectCards[i].card.getName();
        choices.push_back(c);
    }

    json state = buildStateJson(gc);
    state["selectType"] = selectTypeName(gc.info.selectScreenType);
    emitLine("CARD_SELECT", state, choices);

    int defaultIdx = choices.empty() ? 0 : 0;
    int choice = readChoice(defaultIdx);
    gc.chooseSelectCardScreenOption(choice);
}

// ---------------------------------------------------------------------------
// REST_ROOM
// ---------------------------------------------------------------------------

/** Handles REST_ROOM: mirrors ConsoleSimulator's option numbering (0=rest,1=smith,2=recall,3=lift,4=toke,5=dig,6=skip). */
void handleRestRoom(GameContext &gc) {
    json choices = json::array();
    bool hasOption = false;

    if (!gc.relics.has(RelicId::COFFEE_DRIPPER)) {
        hasOption = true;
        json c; c["index"] = 0; c["label"] = "rest"; choices.push_back(c);
    }
    if (!gc.relics.has(RelicId::FUSION_HAMMER) && gc.deck.getUpgradeableCount() > 0) {
        hasOption = true;
        json c; c["index"] = 1; c["label"] = "smith"; choices.push_back(c);
    }
    if (!gc.hasKey(Key::RUBY_KEY)) {
        hasOption = true;
        json c; c["index"] = 2; c["label"] = "recall"; choices.push_back(c);
    }
    if (gc.relics.has(RelicId::GIRYA) && gc.relics.getRelicValue(RelicId::GIRYA) != 3) {
        hasOption = true;
        json c; c["index"] = 3; c["label"] = "lift"; choices.push_back(c);
    }
    if (gc.relics.has(RelicId::PEACE_PIPE)) {
        hasOption = true;
        json c; c["index"] = 4; c["label"] = "toke"; choices.push_back(c);
    }
    if (gc.relics.has(RelicId::SHOVEL)) {
        hasOption = true;
        json c; c["index"] = 5; c["label"] = "dig"; choices.push_back(c);
    }
    if (!hasOption) {
        json c; c["index"] = 6; c["label"] = "skip"; choices.push_back(c);
    }

    emitLine("REST", buildStateJson(gc), choices);

    // --auto default: smith if available else rest else skip.
    int defaultIdx = 6;
    for (const auto &c : choices) {
        if (c["label"] == "smith") { defaultIdx = 1; break; }
    }
    if (defaultIdx == 6) {
        for (const auto &c : choices) {
            if (c["label"] == "rest") { defaultIdx = 0; break; }
        }
    }

    int choice = readChoice(defaultIdx);
    gc.chooseCampfireOption(choice);
}

// ---------------------------------------------------------------------------
// SHOP_ROOM
// ---------------------------------------------------------------------------

/**
 * Handles SHOP_ROOM: lists all non-sold items (card/relic/potion/remove) with
 * price, plus "proceed". Purchases keep the screen in SHOP_ROOM (multiple
 * buys allowed), so this loops emitting fresh lines until "proceed".
 */
void handleShopRoom(GameContext &gc) {
    while (gc.screenState == ScreenState::SHOP_ROOM) {
        auto &shop = gc.info.shop;
        json choices = json::array();
        int idxCounter = 0;

        // encode choice index -> (kind, subIdx) via parallel arrays since the
        // wire index must be a flat 0..N-1 the driver echoes back.
        std::vector<std::pair<std::string,int>> mapping;

        for (int i = 0; i < 7; ++i) {
            int price = shop.cardPrice(i);
            if (price != -1) {
                json c = cardToJson(shop.cards[i]);
                c["index"] = idxCounter;
                c["price"] = price;
                c["label"] = std::string("Buy card: ") + shop.cards[i].getName();
                choices.push_back(c);
                mapping.emplace_back("card", i);
                ++idxCounter;
            }
        }
        for (int i = 0; i < 3; ++i) {
            int price = shop.relicPrice(i);
            if (price != -1) {
                json c;
                c["index"] = idxCounter;
                c["price"] = price;
                c["label"] = std::string("Buy relic: ") + getRelicName(shop.relics[i]);
                choices.push_back(c);
                mapping.emplace_back("relic", i);
                ++idxCounter;
            }
        }
        for (int i = 0; i < 3; ++i) {
            int price = shop.potionPrice(i);
            if (price != -1) {
                json c;
                c["index"] = idxCounter;
                c["price"] = price;
                c["label"] = std::string("Buy potion: ") + getPotionName(shop.potions[i]);
                choices.push_back(c);
                mapping.emplace_back("potion", i);
                ++idxCounter;
            }
        }
        if (shop.removeCost != -1) {
            json c;
            c["index"] = idxCounter;
            c["price"] = shop.removeCost;
            c["label"] = "Remove a card";
            choices.push_back(c);
            mapping.emplace_back("remove", 0);
            ++idxCounter;
        }

        int proceedIdx = idxCounter;
        json proceed;
        proceed["index"] = proceedIdx;
        proceed["label"] = "proceed";
        choices.push_back(proceed);
        mapping.emplace_back("proceed", 0);

        emitLine("SHOP", buildStateJson(gc), choices);

        // --auto default: proceed immediately (buy nothing).
        int choice = readChoice(proceedIdx);
        if (choice < 0 || choice >= static_cast<int>(mapping.size())) {
            choice = proceedIdx;
        }

        const auto &[kind, subIdx] = mapping[choice];
        if (kind == "card") {
            shop.buyCard(gc, subIdx);
        } else if (kind == "relic") {
            shop.buyRelic(gc, subIdx);
        } else if (kind == "potion") {
            shop.buyPotion(gc, subIdx);
        } else if (kind == "remove") {
            shop.buyCardRemove(gc);
        } else { // proceed
            gc.screenState = ScreenState::MAP_SCREEN;
        }
    }
}

// ---------------------------------------------------------------------------
// TREASURE_ROOM
// ---------------------------------------------------------------------------

/** Handles TREASURE_ROOM: 0=open chest, 1=skip. */
void handleTreasureRoom(GameContext &gc) {
    json choices = json::array();
    json openC; openC["index"] = 0; openC["label"] = std::string("Open ") + chestSizeNames[static_cast<int>(gc.info.chestSize)] + " chest"; choices.push_back(openC);
    json skipC; skipC["index"] = 1; skipC["label"] = "Skip Reward"; choices.push_back(skipC);

    emitLine("TREASURE", buildStateJson(gc), choices);

    int choice = readChoice(0); // --auto default: open
    gc.chooseTreasureRoomOption(choice == 0);
}

// ---------------------------------------------------------------------------
// EVENT_SCREEN
// ---------------------------------------------------------------------------

/**
 * Handles EVENT_SCREEN. Neow uses the dedicated chooseNeowOption() API (a
 * different call than every other event). For all other events we enumerate
 * options generically using terse labels (rather than reproducing every
 * event's full ConsoleSimulator text) since the driver applies simple
 * defaults; complex/rare events (Match and Keep, Falling, We Meet Again) get
 * simplified, best-effort option lists -- noted inline below.
 */
void handleEventScreen(GameContext &gc) {
    if (gc.curEvent == Event::NEOW) {
        const auto &r = gc.info.neowRewards;
        json choices = json::array();
        for (int i = 0; i < 4; ++i) {
            json c;
            c["index"] = i;
            std::string label = Neow::bonusStrings[static_cast<int>(r[i].r)];
            std::string drawback = Neow::drawbackStrings[static_cast<int>(r[i].d)];
            if (drawback != "NONE" && !drawback.empty()) {
                label += " / " + drawback;
            }
            c["label"] = label;
            choices.push_back(c);
        }
        // Include the full act map at Neow (floor 0) so the driver's Neow policy can read it — the
        // enemies-1-HP snipe conditional and the curse/early-shop conditional both need it.
        json neowState = buildStateJson(gc);
        neowState["map"] = mapToJson(gc);
        emitLine("NEOW", neowState, choices);
        int choice = readChoice(0);
        if (choice < 0 || choice > 3) choice = 0;
        gc.chooseNeowOption(gc.info.neowRewards[choice]);
        return;
    }

    if (gc.curEvent == Event::MATCH_AND_KEEP) {
        // Simplified: Match and Keep normally requires picking TWO card
        // indices per attempt (chooseMatchAndKeepCards(idx1, idx2)). We
        // present the visible (deckIdx != 0) options and always submit the
        // first two visible indices found -- a best-effort default rather
        // than modeling full memory-match strategy.
        json choices = json::array();
        std::vector<int> visibleIdxs;
        for (int i = 0; i < gc.info.toSelectCards.size(); ++i) {
            const auto &c = gc.info.toSelectCards[i];
            if (c.deckIdx == 0) continue;
            json cj;
            cj["index"] = i;
            cj["label"] = c.card.getName();
            choices.push_back(cj);
            visibleIdxs.push_back(i);
        }
        json state = buildStateJson(gc);
        state["eventName"] = eventGameNames[static_cast<int>(gc.curEvent)];
        emitLine("EVENT", state, choices);
        int choice1 = readChoice(visibleIdxs.empty() ? 0 : visibleIdxs[0]);
        int choice2 = choice1;
        if (!g_auto) {
            // Driver is expected to send a second {"choose": idx2} line for
            // the second card of the pair.
            choice2 = readChoice(visibleIdxs.size() > 1 ? visibleIdxs[1] : choice1);
        } else if (visibleIdxs.size() > 1) {
            choice2 = visibleIdxs[1];
        }
        gc.chooseMatchAndKeepCards(choice1, choice2);
        return;
    }

    // Generic case: enumerate options with terse labels. We do not attempt to
    // reproduce every event's exact bespoke text (see ConsoleSimulator's huge
    // switch for that); indices below follow ConsoleSimulator's numbering
    // where practical so both are cross-checkable, but gaps in numbering are
    // collapsed to a dense 0..N-1 choice list since only presented options
    // are legal to send back via chooseEventOption().
    json choices = json::array();
    const bool unfavorable = gc.ascension >= 15;

    auto addChoice = [&](int idx, const std::string &label) {
        json c;
        c["index"] = idx;
        c["label"] = label;
        choices.push_back(c);
    };

    switch (gc.curEvent) {
        case Event::OMINOUS_FORGE:
            if (gc.deck.getUpgradeableCount() > 0) addChoice(0, "Forge: upgrade a card");
            addChoice(1, "Rummage: Warped Tongs + Curse");
            addChoice(2, "Leave");
            break;
        case Event::PLEADING_VAGRANT:
            if (gc.gold >= 85) addChoice(0, "Give 85 Gold for a relic");
            addChoice(1, "Rob: relic + curse");
            addChoice(2, "Leave");
            break;
        case Event::ANCIENT_WRITING:
            addChoice(0, "Elegance: remove a card");
            addChoice(1, "Simplicity: upgrade all Strikes/Defends");
            break;
        case Event::OLD_BEGGAR:
            addChoice(0, "Offer Gold: lose 75g, remove a card");
            addChoice(1, "Leave");
            break;
        case Event::BIG_FISH:
            addChoice(0, "Banana: heal");
            addChoice(1, "Donut: +5 max HP");
            addChoice(2, "Box: relic + curse");
            break;
        case Event::COLOSSEUM:
            if (gc.info.eventData == 0) {
                addChoice(0, "Fight");
            } else {
                addChoice(0, "Escape (cowardice)");
                addChoice(1, "Victory fight");
            }
            break;
        case Event::CURSED_TOME: {
            const int phase = gc.info.eventData;
            switch (phase) {
                case 0: addChoice(0, "Read"); addChoice(1, "Leave"); break;
                case 1: case 2: case 3: addChoice(phase + 1, "Continue"); break;
                case 4: addChoice(5, "Take the Book"); addChoice(6, "Stop"); break;
                default: break;
            }
            break;
        }
        case Event::DEAD_ADVENTURER:
            addChoice(0, "Search");
            addChoice(1, "Escape");
            break;
        case Event::DESIGNER_IN_SPIRE: {
            const bool upgradeOne = gc.info.upgradeOne;
            const bool cleanUpIsRemoveCard = gc.info.cleanUpIsRemoveCard;
            int goldCost0 = unfavorable ? 50 : 40;
            if (gc.gold >= goldCost0 && gc.deck.getUpgradeableCount() > 0) {
                addChoice(upgradeOne ? 0 : 1, upgradeOne ? "Adjustments: upgrade a card" : "Adjustments: upgrade 2 random cards");
            }
            int goldCost1 = unfavorable ? 75 : 60;
            if (gc.gold >= goldCost1) {
                if (cleanUpIsRemoveCard) {
                    if (gc.deck.getTransformableCount(1) >= 1) addChoice(2, "Clean Up: remove a card");
                } else {
                    if (gc.deck.getTransformableCount(2) >= 2) addChoice(3, "Clean Up: transform 2 random cards");
                }
            }
            int goldCost2 = unfavorable ? 110 : 90;
            if (gc.gold >= goldCost2 && gc.deck.getTransformableCount(1) >= 1) {
                addChoice(4, "Full Service: remove + upgrade");
            }
            addChoice(5, "Punch: lose HP");
            break;
        }
        case Event::AUGMENTER:
            addChoice(0, "Test J.A.X.");
            if (gc.deck.getTransformableCount(2) > 1) addChoice(1, "Become Test Subject: transform 2 cards");
            addChoice(2, "Ingest Mutagens: Mutagenic Strength");
            break;
        case Event::DUPLICATOR:
            addChoice(0, "Pray: duplicate a card");
            addChoice(1, "Leave");
            break;
        case Event::FACE_TRADER:
            addChoice(0, "Touch: lose HP, gain gold");
            addChoice(1, "Trade: 50/50 face");
            addChoice(2, "Leave");
            break;
        case Event::FALLING: {
            // Simplified: options depend on which card categories exist in
            // the deck at fixed slots; we present whichever are available.
            if (gc.info.skillCardDeckIdx != -1) addChoice(0, "Land: lose a Skill");
            if (gc.info.powerCardDeckIdx != -1) addChoice(1, "Channel: lose a Power");
            if (gc.info.attackCardDeckIdx != -1) addChoice(2, "Strike: lose an Attack");
            if (gc.info.skillCardDeckIdx == -1 && gc.info.powerCardDeckIdx == -1 && gc.info.attackCardDeckIdx == -1) {
                addChoice(3, "Splat: lose nothing");
            }
            break;
        }
        case Event::FORGOTTEN_ALTAR:
            if (gc.relics.has(RelicId::GOLDEN_IDOL)) addChoice(0, "Offer Golden Idol");
            addChoice(1, "Sacrifice: +5 max HP, lose HP");
            addChoice(2, "Desecrate: Curse");
            break;
        case Event::THE_DIVINE_FOUNTAIN:
            addChoice(0, "Drink: remove all curses");
            addChoice(1, "Leave");
            break;
        case Event::GHOSTS:
            addChoice(0, "Accept: Apparitions, lose max HP");
            addChoice(1, "Refuse");
            break;
        case Event::GOLDEN_IDOL:
            if (!gc.relics.has(RelicId::GOLDEN_IDOL)) {
                addChoice(0, "Take: Golden Idol + trap");
                addChoice(1, "Leave");
            } else {
                addChoice(2, "Outrun: Curse");
                addChoice(3, "Smash: take damage");
                addChoice(4, "Hide: lose max HP");
            }
            break;
        case Event::GOLDEN_SHRINE:
            addChoice(0, "Pray: gold");
            addChoice(1, "Desecrate: more gold + curse");
            addChoice(2, "Leave");
            break;
        case Event::WING_STATUE:
            addChoice(0, "Pray: remove a card, lose HP");
            if (gc.deck.hasCardForWingStatue()) addChoice(1, "Destroy: gold");
            addChoice(2, "Leave");
            break;
        case Event::KNOWING_SKULL:
            addChoice(0, "Riches: gold, lose HP");
            addChoice(1, "Success: colorless card, lose HP");
            addChoice(2, "Pick Me Up: potion, lose HP");
            addChoice(3, "Leave: lose HP");
            break;
        case Event::THE_SSSSSERPENT:
            addChoice(0, "Agree: gold + curse");
            addChoice(1, "Disagree");
            break;
        case Event::LIVING_WALL:
            addChoice(0, "Forget: remove a card");
            addChoice(1, "Change: transform a card");
            if (gc.deck.getUpgradeableCount() > 0) addChoice(2, "Grow: upgrade a card");
            break;
        case Event::MASKED_BANDITS:
            addChoice(0, "Pay: lose all gold");
            addChoice(1, "Fight");
            break;
        case Event::MINDBLOOM:
            addChoice(0, "I am War: fight Act 1 boss");
            addChoice(1, "I am Awake: upgrade all cards, no more healing");
            if (gc.floorNum <= 40) addChoice(2, "I am Rich: gold + curse");
            else addChoice(3, "I am Healthy: full heal + curse");
            break;
        case Event::HYPNOTIZING_COLORED_MUSHROOMS:
            addChoice(0, "Stomp: anger the mushrooms");
            addChoice(1, "Eat: heal + curse");
            break;
        case Event::MYSTERIOUS_SPHERE:
            addChoice(0, "Open Sphere: fight for rare relic");
            addChoice(1, "Leave");
            break;
        case Event::THE_NEST:
            addChoice(0, "Smash and Grab: gold");
            addChoice(1, "Stay in Line: Ritual Dagger, lose HP");
            break;
        case Event::NLOTH:
            addChoice(0, "Offer first relic");
            addChoice(1, "Offer second relic");
            addChoice(2, "Leave");
            break;
        case Event::NOTE_FOR_YOURSELF:
            addChoice(0, "Take and Give");
            addChoice(1, "Ignore");
            break;
        case Event::PURIFIER:
            if (gc.deck.getTransformableCount(1) > 0) addChoice(0, "Pray: remove a card");
            addChoice(1, "Leave");
            break;
        case Event::SCRAP_OOZE:
            addChoice(0, "Reach Inside: lose HP, chance of relic");
            addChoice(1, "Leave");
            break;
        case Event::SECRET_PORTAL:
            addChoice(0, "Enter the Portal: skip to boss");
            addChoice(1, "Leave");
            break;
        case Event::SENSORY_STONE:
            addChoice(0, "Recall: +1 colorless card");
            addChoice(1, "Recall: +2 colorless cards, lose HP");
            addChoice(2, "Recall: +3 colorless cards, lose more HP");
            break;
        case Event::SHINING_LIGHT:
            addChoice(0, "Enter: upgrade 2 random cards, take damage");
            addChoice(1, "Leave");
            break;
        case Event::THE_CLERIC:
            addChoice(0, "Heal: lose 35g");
            if (gc.gold >= (unfavorable ? 75 : 50)) addChoice(1, "Purify: remove a card");
            addChoice(2, "Leave");
            break;
        case Event::THE_JOUST:
            addChoice(0, "Bet on Murderer");
            addChoice(1, "Bet on Owner");
            break;
        case Event::THE_LIBRARY:
            addChoice(0, "Read: choose a card");
            addChoice(1, "Sleep: heal");
            break;
        case Event::THE_MAUSOLEUM:
            addChoice(0, "Open Coffin: relic, maybe curse");
            addChoice(1, "Leave");
            break;
        case Event::THE_MOAI_HEAD:
            addChoice(0, "Jump Inside: full heal, lose max HP");
            if (gc.hasRelic(RelicId::GOLDEN_IDOL)) addChoice(1, "Offer Golden Idol: gold");
            addChoice(2, "Leave");
            break;
        case Event::THE_WOMAN_IN_BLUE:
            addChoice(0, "Buy 1 Potion");
            addChoice(1, "Buy 2 Potions");
            addChoice(2, "Buy 3 Potions");
            addChoice(3, "Leave");
            break;
        case Event::TOMB_OF_LORD_RED_MASK:
            if (gc.hasRelic(RelicId::RED_MASK)) addChoice(0, "Don the Red Mask: gold");
            else addChoice(1, "Offer all gold for Red Mask");
            addChoice(2, "Leave");
            break;
        case Event::TRANSMORGRIFIER:
            if (gc.deck.getTransformableCount(1) > 0) addChoice(0, "Pray: transform a card");
            addChoice(1, "Leave");
            break;
        case Event::UPGRADE_SHRINE:
            if (gc.deck.getUpgradeableCount() > 0) addChoice(0, "Pray: upgrade a card");
            addChoice(1, "Leave");
            break;
        case Event::VAMPIRES:
            if (gc.relics.has(RelicId::BLOOD_VIAL)) addChoice(0, "Offer Blood Vial: Strikes -> Bites");
            addChoice(1, "Accept: Strikes -> Bites, lose max HP");
            addChoice(2, "Refuse");
            break;
        case Event::WE_MEET_AGAIN:
            if (gc.info.potionIdx != -1) addChoice(0, "Give Potion");
            if (gc.info.gold != -1) addChoice(1, "Give Gold");
            if (gc.info.cardIdx != -1) addChoice(2, "Give Card");
            addChoice(3, "Attack");
            break;
        case Event::WHEEL_OF_CHANGE:
            addChoice(0, "Play: spin the wheel");
            break;
        case Event::WINDING_HALLS:
            addChoice(0, "Embrace Madness");
            addChoice(1, "Press On: curse + heal");
            addChoice(2, "Retrace Your Steps: lose max HP");
            break;
        case Event::WORLD_OF_GOOP:
            addChoice(0, "Gather Gold: gold, lose HP");
            addChoice(1, "Leave It");
            break;
        default:
            // Unhandled/rare event type: fall back to a single "continue"
            // option at index 0, which is the common no-op path for most
            // events not explicitly listed above.
            addChoice(0, "Continue");
            break;
    }

    json state = buildStateJson(gc);
    // The event's in-game display name (eventGameNames), so the TS event resolver can key the KB advice
    // table on it. The driver normalizes it to the KB's `eventName` spelling.
    state["eventName"] = eventGameNames[static_cast<int>(gc.curEvent)];
    emitLine("EVENT", state, choices);

    int defaultIdx = choices.empty() ? 0 : choices[0]["index"].get<int>();
    int choice = readChoice(defaultIdx);
    gc.chooseEventOption(choice);
}

// ---------------------------------------------------------------------------
// COMBAT DISPATCH
// ---------------------------------------------------------------------------

/**
 * Resolves one battle with the MCTS agent under a wall-clock watchdog (see g_combatTimeoutMs):
 * runs ScumSearchAgent2::playoutBattle on a private copy of `bc` on a worker thread; if it
 * finishes within budget, commits the copy back into `bc` and returns true. If the deadline
 * passes (a pathological battle state that never terminates under BattleScumSearcher2's random
 * rollout -- see g_combatTimeoutMs's doc comment), the worker thread is detached (it keeps
 * running harmlessly to completion on its own orphaned copy; the process eventually exits at
 * emitGameOverAndExit so it never blocks shutdown) and this returns false, leaving the original
 * `bc` untouched for the caller to resolve some other way (SimpleAgent fallback).
 */
bool tryPlayoutBattleScum(search::ScumSearchAgent2 &agent, BattleContext &bc) {
    auto resultHolder = std::make_shared<BattleContext>(bc);
    auto done = std::make_shared<std::atomic<bool>>(false);

    std::thread worker([&agent, resultHolder, done]() {
        agent.playoutBattle(*resultHolder);
        done->store(true, std::memory_order_release);
    });

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(g_combatTimeoutMs);
    while (!done->load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    if (done->load(std::memory_order_acquire)) {
        worker.join();
        bc = *resultHolder;
        return true;
    }

    std::cerr << "jsonbridge: --combat=scum battle exceeded " << g_combatTimeoutMs
              << "ms watchdog on floor " << bc.floorNum << " (" << monsterEncounterStrings[static_cast<int>(bc.encounter)]
              << "); falling back to SimpleAgent for this fight" << std::endl;
    worker.detach(); // orphaned copy keeps running to completion harmlessly; process exit doesn't wait on it
    return false;
}

/** Resolves one battle: --combat=scum with a watchdog + SimpleAgent fallback, else plain SimpleAgent. */
void resolveBattle(CombatMode mode, search::SimpleAgent &simpleAgent, search::ScumSearchAgent2 &scumAgent, BattleContext &bc) {
    if (mode == CombatMode::SCUM && tryPlayoutBattleScum(scumAgent, bc)) {
        return;
    }
    simpleAgent.playoutBattle(bc);
}

} // namespace

int main(int argc, char **argv) {
    if (argc < 4) {
        std::cerr << "usage: jsonbridge <seedString> <I|S|D|W> <ascensionLevel> [--auto] "
                      "[--combat=simple|scum] [--combat-sims=N] [--combat-timeout-ms=N]" << std::endl;
        return 1;
    }

    std::string seedStr = argv[1];
    char ch = argv[2][0];
    int ascension = std::stoi(argv[3]);

    for (int i = 4; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--auto") {
            g_auto = true;
        } else if (arg.rfind("--combat=", 0) == 0) {
            std::string mode = arg.substr(9);
            if (mode == "simple") {
                g_combatMode = CombatMode::SIMPLE;
            } else if (mode == "scum") {
                g_combatMode = CombatMode::SCUM;
            } else {
                std::cerr << "jsonbridge: bad --combat mode '" << mode << "' (want simple|scum)" << std::endl;
                return 1;
            }
        } else if (arg.rfind("--combat-sims=", 0) == 0) {
            g_combatSims = std::stoll(arg.substr(14));
        } else if (arg.rfind("--combat-timeout-ms=", 0) == 0) {
            g_combatTimeoutMs = std::stoll(arg.substr(20));
        }
    }

    CharacterClass cc;
    switch (ch) {
        case 'I': case 'i': cc = CharacterClass::IRONCLAD; break;
        case 'S': case 's': cc = CharacterClass::SILENT; break;
        case 'D': case 'd': cc = CharacterClass::DEFECT; break;
        case 'W': case 'w': cc = CharacterClass::WATCHER; break;
        default:
            std::cerr << "jsonbridge: bad character '" << ch << "'" << std::endl;
            return 1;
    }

    std::uint64_t seed = SeedHelper::getLong(seedStr);
    GameContext gc(cc, seed, ascension);
    // IMPORTANT: do NOT set gc.skipBattles = true -- combat must be resolved
    // for real via the chosen battle agent, not skipped to an instant win.

    search::SimpleAgent simpleBattleAgent; // constructed once, reused across the whole run (see SimpleAgent::playout)
    search::ScumSearchAgent2 scumBattleAgent; // constructed once; playoutBattle rebuilds its searcher per decision
    scumBattleAgent.simulationCountBase = g_combatSims;
    // ScumSearchAgent2::rng is only used by its own out-of-combat policy helpers, which jsonbridge
    // never calls (out-of-combat decisions are driven by this file's handle*() functions instead).
    // Determinism-per-seed for --combat=scum comes from BattleScumSearcher2's own randGen, which its
    // constructor seeds from `bc.seed + bc.floorNum` (see BattleScumSearcher2.cpp) -- deterministic
    // given the run seed, with no wall-clock dependency. We still seed `rng` from the run seed for
    // good measure / future-proofing against any as-yet-unused RNG consumers.
    scumBattleAgent.rng = std::default_random_engine(static_cast<std::default_random_engine::result_type>(seed));

    while (gc.outcome == GameOutcome::UNDECIDED) {
        switch (gc.screenState) {
            case ScreenState::BATTLE: {
                BattleContext bc;
                bc.init(gc);
                resolveBattle(g_combatMode, simpleBattleAgent, scumBattleAgent, bc); // no JSON line ever printed for combat
                bc.exitBattle(gc);
                break;
            }
            case ScreenState::MAP_SCREEN:
                handleMapScreen(gc);
                break;
            case ScreenState::REWARDS:
                handleRewards(gc);
                break;
            case ScreenState::BOSS_RELIC_REWARDS:
                handleBossRelicRewards(gc);
                break;
            case ScreenState::CARD_SELECT:
                handleCardSelect(gc);
                break;
            case ScreenState::REST_ROOM:
                handleRestRoom(gc);
                break;
            case ScreenState::SHOP_ROOM:
                handleShopRoom(gc);
                break;
            case ScreenState::TREASURE_ROOM:
                handleTreasureRoom(gc);
                break;
            case ScreenState::EVENT_SCREEN:
                handleEventScreen(gc);
                break;
            case ScreenState::INVALID:
            default:
                std::cerr << "jsonbridge: unexpected screenState "
                          << static_cast<int>(gc.screenState) << std::endl;
                std::exit(1);
        }

        if (gc.outcome != GameOutcome::UNDECIDED) {
            break;
        }
    }

    emitGameOverAndExit(gc);
    return 0;
}
