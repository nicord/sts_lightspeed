// Fidelity harness: drive sts_lightspeed's GameContext directly (bypassing the
// text ConsoleSimulator) for a given seed/ascension/character/neow-choice/path,
// with skipBattles=true so combat resolves instantly and we can read off the
// generated map / Neow options / card rewards / boss identity as JSON.
//
// Usage: fidelity_harness <seedString> <I/S/D/W> <ascension> <neowChoiceIdx> <x0> <x1> ... <xN>
//   xI = the map-node x coordinate chosen at floor i+1 (1-based floor), i.e. path taken.
//   Prints one JSON object to stdout.
//
// Link against the same sources as apps/main (see build_harness.sh).

#include <iostream>
#include <sstream>
#include <vector>
#include <string>

#include "game/GameContext.h"
#include "game/Game.h"
#include "game/Map.h"
#include "game/Neow.h"

using namespace sts;

static std::string jesc(const std::string &s) {
    std::string out;
    for (char c : s) {
        if (c == '"' || c == '\\') out += '\\';
        out += c;
    }
    return out;
}

int main(int argc, char **argv) {
    if (argc < 5) {
        std::cerr << "usage: fidelity_harness <seedString> <I/S/D/W> <ascension> <neowChoiceIdx> [x1 x2 ...]\n";
        return 1;
    }

    std::string seedStr = argv[1];
    char ch = argv[2][0];
    int ascension = std::stoi(argv[3]);
    int neowChoiceIdx = std::stoi(argv[4]);

    std::vector<int> path;
    for (int i = 5; i < argc; ++i) path.push_back(std::stoi(argv[i]));

    CharacterClass cc;
    switch (ch) {
        case 'I': cc = CharacterClass::IRONCLAD; break;
        case 'S': cc = CharacterClass::SILENT; break;
        case 'D': cc = CharacterClass::DEFECT; break;
        case 'W': cc = CharacterClass::WATCHER; break;
        default: std::cerr << "bad character\n"; return 1;
    }

    std::uint64_t seed = SeedHelper::getLong(seedStr);

    std::cout << "{\n";
    std::cout << "  \"seed_string_in\": \"" << jesc(seedStr) << "\",\n";
    std::cout << "  \"seed_numeric_computed\": " << seed << ",\n";
    std::cout << "  \"seed_string_roundtrip\": \"" << jesc(SeedHelper::getString(seed)) << "\",\n";

    GameContext gc(cc, seed, ascension);
    gc.skipBattles = true;

    std::cout << "  \"boss\": \"" << jesc(monsterEncounterStrings[static_cast<int>(gc.boss)]) << "\",\n";

    // ---- Neow options ----
    std::cout << "  \"neow_options\": [\n";
    for (int i = 0; i < 4; ++i) {
        const auto &opt = gc.info.neowRewards[i];
        std::cout << "    {\"bonus\": \"" << jesc(Neow::bonusStrings[static_cast<int>(opt.r)])
                   << "\", \"drawback\": \"" << jesc(Neow::drawbackStrings[static_cast<int>(opt.d)]) << "\"}";
        std::cout << (i < 3 ? ",\n" : "\n");
    }
    std::cout << "  ],\n";

    // ---- Map (Act 1, matches gc's initial map from ctor) ----
    std::cout << "  \"map\": [\n";
    bool first = true;
    for (int y = 0; y < 15; ++y) {
        for (int x = 0; x < 7; ++x) {
            const MapNode &n = gc.map->getNode(x, y);
            if (n.room == Room::NONE) continue;
            if (!first) std::cout << ",\n";
            first = false;
            std::cout << "    {\"x\": " << x << ", \"y\": " << y
                      << ", \"symbol\": \"" << getRoomSymbol(n.room) << "\"}";
        }
    }
    std::cout << "\n  ],\n";

    // ---- choose Neow option ----
    gc.chooseNeowOption(gc.info.neowRewards[neowChoiceIdx]);

    // gc.screenState should now be MAP_SCREEN (or a card-select/rewards screen for
    // some bonuses, e.g. THREE_CARDS/UPGRADE_CARD -- we only auto-resolve MAP_SCREEN
    // and REWARDS/CARD_SELECT by taking sensible defaults so we can keep walking the path).

    auto resolveNonMapScreens = [&]() {
        int guard = 0;
        while (gc.screenState != ScreenState::MAP_SCREEN && gc.screenState != ScreenState::BATTLE && guard < 20) {
            ++guard;
            switch (gc.screenState) {
                case ScreenState::REWARDS:
                    // just leave rewards screen (simulate "proceed")
                    gc.regainControl();
                    break;
                case ScreenState::CARD_SELECT:
                    // pick first available card / option to unblock
                    gc.chooseSelectCardScreenOption(0);
                    break;
                case ScreenState::EVENT_SCREEN:
                    if (gc.curEvent == Event::NEOW) {
                        // We already resolved Neow explicitly (with the caller-supplied
                        // choice) before entering this loop; this is just the trailing
                        // "Leave"/regain-control screen, not a fresh choice.
                        gc.regainControl();
                    } else {
                        gc.chooseEventOption(0);
                    }
                    break;
                case ScreenState::REST_ROOM:
                    gc.chooseCampfireOption(0);
                    break;
                case ScreenState::SHOP_ROOM:
                    gc.regainControl();
                    break;
                case ScreenState::TREASURE_ROOM:
                    gc.chooseTreasureRoomOption(false);
                    break;
                case ScreenState::BOSS_RELIC_REWARDS:
                    gc.chooseBossRelic(0);
                    break;
                default:
                    guard = 999;
                    break;
            }
        }
    };

    resolveNonMapScreens();

    // ---- card rewards collected along recorded path ----
    std::cout << "  \"card_rewards\": [\n";
    bool firstReward = true;
    for (size_t i = 0; i < path.size(); ++i) {
        if (gc.screenState != ScreenState::MAP_SCREEN) {
            std::cerr << "WARN: not at map screen before floor " << (i+1) << ", state=" << static_cast<int>(gc.screenState) << "\n";
            break;
        }
        gc.transitionToMapNode(path[i]);

        // battle rooms: with skipBattles=true, enterBattle() calls afterBattle() immediately,
        // which (for Room::MONSTER) opens the combat reward screen.
        if (gc.screenState == ScreenState::REWARDS) {
            const auto &rc = gc.info.rewardsContainer;
            if (!firstReward) std::cout << ",\n";
            firstReward = false;
            std::cout << "    {\"floor\": " << gc.floorNum << ", \"cards\": [";
            bool firstCard = true;
            for (int ci = 0; ci < rc.cardRewardCount; ++ci) {
                const auto &reward = rc.cardRewards[ci];
                for (const auto &card : reward) {
                    if (!firstCard) std::cout << ", ";
                    firstCard = false;
                    std::cout << "\"" << jesc(card.getName()) << "\"";
                }
            }
            std::cout << "]}";
        }

        resolveNonMapScreens();
    }
    std::cout << "\n  ]\n";

    std::cout << "}\n";
    return 0;
}
