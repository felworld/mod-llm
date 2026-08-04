/*
 * This file is part of mod-llm for Felworld. Released under the MIT license
 * (see the LICENSE file at the module root).
 */

#include "BattlegroundContext.h"

#include "Battleground.h"
#include "BattlegroundAB.h"
#include "BattlegroundAV.h"
#include "BattlegroundEY.h"
#include "BattlegroundIC.h"
#include "BattlegroundSA.h"
#include "BattlegroundWS.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "StringFormat.h"
#include "World.h"

#include <algorithm>
#include <vector>

namespace ModLlm::BattlegroundContext
{
    namespace
    {
        // One battleground as its HUD reads. `state` stays empty until the
        // gates open - before that the scoreboard is all zeroes, and handing
        // the model a row of zeroes only invites it to comment on nothing.
        // `shorthand` is the callout key: the names and abbreviations players
        // actually type in this map, which a bot needs to read team chat.
        struct MatchFacts
        {
            char const* name = "";
            std::string rules;
            std::string state;
            std::string shorthand;
        };

        std::string JoinNames(std::vector<std::string> const& names)
        {
            std::string joined;
            for (std::size_t i = 0; i < names.size(); ++i)
            {
                if (i)
                    joined += i + 1 == names.size() ? " and " : ", ";
                joined += names[i];
            }
            return joined;
        }

        // Scoreboard clauses into one sentence: "you hold X; they hold Y; Z is
        // still unclaimed."
        std::string JoinClauses(std::vector<std::string> const& clauses)
        {
            std::string joined;
            for (std::string const& clause : clauses)
            {
                if (!joined.empty())
                    joined += "; ";
                joined += clause;
            }
            return joined + ".";
        }

        // Win thresholds are server-configurable, and a player reads the real
        // one off their scoreboard - so must the bot.
        uint32 ConfiguredTarget(ServerConfigs option, uint32 fallback)
        {
            uint32 configured = sWorld->getIntConfig(option);
            return configured ? configured : fallback;
        }

        // Only Warsong Gulch and Strand show a clock. Everywhere else pace is
        // read off how long the match has run against how the score moved,
        // which is what a player has too (their own sense of time, not a
        // number on screen) - so it stays coarse on purpose.
        std::string PaceClause(Battleground* bg)
        {
            uint32 prepTime = sWorld->getIntConfig(CONFIG_BATTLEGROUND_PREP_TIME) * IN_MILLISECONDS;
            uint32 elapsed = bg->GetStartTime() > prepTime ? bg->GetStartTime() - prepTime : 0;
            uint32 minutes = elapsed / (MINUTE * IN_MILLISECONDS);
            if (minutes < 2)
                return "the match only just started";
            return Acore::StringFormat("roughly {} minutes in", minutes);
        }

        std::string PlayerName(ObjectGuid guid)
        {
            Player* player = ObjectAccessor::FindPlayer(guid);
            return player ? player->GetName() : "someone";
        }

        MatchFacts WarsongGulch(Player* bot, Battleground* bg, bool inProgress)
        {
            BattlegroundWS* ws = static_cast<BattlegroundWS*>(bg);
            TeamId myTeam = bot->GetTeamId();
            TeamId enemyTeam = myTeam == TEAM_ALLIANCE ? TEAM_HORDE : TEAM_ALLIANCE;

            MatchFacts facts;
            facts.name = "Warsong Gulch";
            facts.rules = Acore::StringFormat("first to {} flag captures wins, and if the clock runs out"
                " the higher score takes it",
                ConfiguredTarget(CONFIG_BATTLEGROUND_WARSONG_FLAGS, BG_WS_MAX_TEAM_SCORE));

            if (inProgress)
            {
                // Flag state and picker are indexed by the flag's owning team;
                // the picker is always on the other team.
                std::string myFlag;
                switch (ws->GetFlagState(myTeam))
                {
                    case BG_WS_FLAG_STATE_ON_PLAYER:
                        myFlag = Acore::StringFormat("your flag was taken by enemy {}",
                            PlayerName(ws->GetFlagPickerGUID(myTeam)));
                        break;
                    case BG_WS_FLAG_STATE_ON_GROUND:
                        myFlag = "your flag is loose on the ground";
                        break;
                    default:
                        myFlag = "your flag is safe in your base";
                        break;
                }

                std::string enemyFlag;
                switch (ws->GetFlagState(enemyTeam))
                {
                    case BG_WS_FLAG_STATE_ON_PLAYER:
                        enemyFlag = Acore::StringFormat("your teammate {} is carrying the enemy flag",
                            PlayerName(ws->GetFlagPickerGUID(enemyTeam)));
                        break;
                    case BG_WS_FLAG_STATE_ON_GROUND:
                        enemyFlag = "the enemy flag is loose on the ground";
                        break;
                    default:
                        enemyFlag = "the enemy flag sits in their base";
                        break;
                }

                facts.state = Acore::StringFormat("Score {}-{} in captures (you-them) with about {} minutes"
                    " left on the clock; {}; {}.",
                    bg->GetTeamScore(myTeam), bg->GetTeamScore(enemyTeam), ws->GetMatchTime(), myFlag, enemyFlag);
            }

            // A flag carrier's play is already decided - it never gets the
            // bg_strategy tool, so it gets marching orders instead of the
            // callout key.
            if (bot->HasAura(BG_WS_SPELL_WARSONG_FLAG) || bot->HasAura(BG_WS_SPELL_SILVERWING_FLAG))
            {
                facts.shorthand = "You are the one carrying the enemy flag: whatever gets called,"
                    " your job is getting it home alive.";
                return facts;
            }

            facts.shorthand = "Teammates call plays here in shorthand: inc means enemies incoming, at your"
                " flag room unless another spot is named, and fc points at a flag carrier, like fc mid or"
                " fc tunnel for where one is. When a callout deserves the team's attention, relay it with"
                " the bg_strategy tool - defend_base for inc at your base, attack_fc to hunt the enemy"
                " carrying your flag, defend_fc to stick with your carrier, attack_base to push their"
                " flag room. Whoever takes up the play announces it in this chat, so the tool call alone"
                " is a full response and staying otherwise silent is normal. You read the game yourself:"
                " relay the calls that make sense to you.";
            return facts;
        }

        MatchFacts ArathiBasin(Player* bot, Battleground* bg, bool inProgress)
        {
            constexpr char const* NODE_NAMES[BG_AB_DYNAMIC_NODES_COUNT] =
            {
                "the Stables", "the Blacksmith", "the Farm", "the Lumber Mill", "the Gold Mine"
            };

            BattlegroundAB* ab = static_cast<BattlegroundAB*>(bg);
            TeamId myTeam = bot->GetTeamId();
            uint32 target = ConfiguredTarget(CONFIG_BATTLEGROUND_ARATHI_CAPTUREPOINTS, BG_AB_MAX_TEAM_SCORE);

            MatchFacts facts;
            facts.name = "Arathi Basin";
            facts.rules = Acore::StringFormat("five bases tick resources for whoever holds them - the more"
                " bases, the faster they tick - and the first side to {} resources wins", target);

            if (inProgress)
            {
                std::vector<std::string> ours, theirs, unclaimed, weTake, theyTake;
                for (uint8 node = 0; node < BG_AB_DYNAMIC_NODES_COUNT; ++node)
                {
                    CaptureABPointInfo const& info = ab->GetCapturePointInfo(node);
                    switch (info._state)
                    {
                        case BG_AB_NODE_STATE_ALLY_CONTESTED:
                            (myTeam == TEAM_ALLIANCE ? weTake : theyTake).emplace_back(NODE_NAMES[node]);
                            break;
                        case BG_AB_NODE_STATE_HORDE_CONTESTED:
                            (myTeam == TEAM_HORDE ? weTake : theyTake).emplace_back(NODE_NAMES[node]);
                            break;
                        case BG_AB_NODE_STATE_ALLY_OCCUPIED:
                        case BG_AB_NODE_STATE_HORDE_OCCUPIED:
                            (info._ownerTeamId == myTeam ? ours : theirs).emplace_back(NODE_NAMES[node]);
                            break;
                        default:
                            unclaimed.emplace_back(NODE_NAMES[node]);
                            break;
                    }
                }

                std::vector<std::string> clauses;
                clauses.push_back(ours.empty() ? "you hold nothing"
                    : Acore::StringFormat("you hold {}", JoinNames(ours)));
                clauses.push_back(theirs.empty() ? "they hold nothing"
                    : Acore::StringFormat("they hold {}", JoinNames(theirs)));
                if (!unclaimed.empty())
                    clauses.push_back(Acore::StringFormat("{} still unclaimed", JoinNames(unclaimed)));
                if (!weTake.empty())
                    clauses.push_back(Acore::StringFormat("your team is capping {} right now", JoinNames(weTake)));
                if (!theyTake.empty())
                    clauses.push_back(Acore::StringFormat("they are capping {} right now", JoinNames(theyTake)));

                facts.state = Acore::StringFormat("Resources {}-{} of {} (you-them), {}: {}",
                    bg->GetTeamScore(myTeam), bg->GetTeamScore(myTeam == TEAM_ALLIANCE ? TEAM_HORDE : TEAM_ALLIANCE),
                    target, PaceClause(bg), JoinClauses(clauses));
            }

            facts.shorthand = "Teammates call the bases in shorthand - st or stables, bs blacksmith, farm,"
                " lm lumber mill, gm gold mine - so \"inc bs\" means enemies are closing on the Blacksmith,"
                " \"2 to lm\" asks two people to go take the Lumber Mill, and a lone \"bs?\" is asking who"
                " is defending it.";
            return facts;
        }

        MatchFacts EyeOfTheStorm(Player* bot, Battleground* bg, bool inProgress)
        {
            constexpr char const* TOWER_NAMES[EY_POINTS_MAX] =
            {
                "Fel Reaver Ruins", "Blood Elf Tower", "Draenei Ruins", "Mage Tower"
            };

            BattlegroundEY* ey = static_cast<BattlegroundEY*>(bg);
            TeamId myTeam = bot->GetTeamId();
            uint32 target = ConfiguredTarget(CONFIG_BATTLEGROUND_EYEOFTHESTORM_CAPTUREPOINTS, BG_EY_MAX_TEAM_SCORE);

            MatchFacts facts;
            facts.name = "Eye of the Storm";
            facts.rules = Acore::StringFormat("four towers tick points for whoever holds them, and running"
                " the flag from the middle to a tower you hold banks a chunk more - first to {} points wins",
                target);

            if (inProgress)
            {
                std::vector<std::string> ours, theirs, unclaimed;
                for (uint8 point = 0; point < EY_POINTS_MAX; ++point)
                {
                    CaptureEYPointInfo const& info = ey->GetCapturePointInfo(point);
                    if (info.IsUncontrolled())
                        unclaimed.emplace_back(TOWER_NAMES[point]);
                    else
                        (info.IsUnderControl(myTeam) ? ours : theirs).emplace_back(TOWER_NAMES[point]);
                }

                std::vector<std::string> clauses;
                clauses.push_back(ours.empty() ? "you hold no towers"
                    : Acore::StringFormat("you hold {}", JoinNames(ours)));
                clauses.push_back(theirs.empty() ? "they hold none"
                    : Acore::StringFormat("they hold {}", JoinNames(theirs)));
                if (!unclaimed.empty())
                    clauses.push_back(Acore::StringFormat("{} unclaimed", JoinNames(unclaimed)));

                switch (ey->GetFlagState())
                {
                    case BG_EY_FLAG_STATE_ON_PLAYER:
                    {
                        Player* carrier = ObjectAccessor::FindPlayer(ey->GetFlagPickerGUID());
                        if (carrier && carrier->GetTeamId() == myTeam)
                            clauses.push_back(Acore::StringFormat("your teammate {} has the flag", carrier->GetName()));
                        else
                            clauses.push_back(Acore::StringFormat("enemy {} has the flag",
                                carrier ? carrier->GetName() : "someone"));
                        break;
                    }
                    case BG_EY_FLAG_STATE_ON_GROUND:
                        clauses.push_back("the flag is loose on the ground");
                        break;
                    default:
                        clauses.push_back("the flag is sitting in the middle waiting to be picked up");
                        break;
                }

                facts.state = Acore::StringFormat("Points {}-{} of {} (you-them), {}: {}",
                    bg->GetTeamScore(myTeam), bg->GetTeamScore(myTeam == TEAM_ALLIANCE ? TEAM_HORDE : TEAM_ALLIANCE),
                    target, PaceClause(bg), JoinClauses(clauses));
            }

            facts.shorthand = "Teammates call the towers in shorthand - fr fel reaver, be blood elf, dr"
                " draenei ruins, mt mage tower - so \"inc dr\" means enemies are closing on Draenei Ruins"
                " and \"flag\" calls track whoever picked it up, like \"flag to mt\" for a runner heading"
                " to the Mage Tower.";
            return facts;
        }

        MatchFacts AlteracValley(Player* bot, Battleground* bg, bool inProgress)
        {
            constexpr char const* GRAVEYARD_NAMES[] =
            {
                "the Stormpike Aid Station", "Stormpike Graveyard", "Stonehearth Graveyard",
                "Snowfall Graveyard", "Iceblood Graveyard", "Frostwolf Graveyard", "the Frostwolf Hut"
            };
            constexpr char const* TOWER_NAMES[] =
            {
                "Dun Baldar South Bunker", "Dun Baldar North Bunker", "Icewing Bunker", "Stonehearth Bunker",
                "Iceblood Tower", "Tower Point", "Frostwolf East Tower", "Frostwolf West Tower"
            };

            BattlegroundAV* av = static_cast<BattlegroundAV*>(bg);
            TeamId myTeam = bot->GetTeamId();
            TeamId enemyTeam = myTeam == TEAM_ALLIANCE ? TEAM_HORDE : TEAM_ALLIANCE;
            uint32 target = ConfiguredTarget(CONFIG_BATTLEGROUND_ALTERAC_REINFORCEMENTS, 600);

            MatchFacts facts;
            facts.name = "Alterac Valley";
            facts.rules = Acore::StringFormat("both sides start with {} reinforcements and bleed them on"
                " every death, every tower burned and every captain killed - run the enemy to zero, or"
                " kill their general in their keep, and it is over",
                target);

            if (inProgress)
            {
                uint32 ourGraves = 0;
                uint32 theirGraves = 0;
                std::vector<std::string> contested;
                for (uint8 node = BG_AV_NODES_FIRSTAID_STATION; node <= BG_AV_NODES_FROSTWOLF_HUT; ++node)
                {
                    BG_AV_NodeInfo const& info = av->GetAVNodeInfo(node);
                    if (info.State == POINT_ASSAULTED)
                        contested.push_back(Acore::StringFormat("{} is being taken by {}",
                            GRAVEYARD_NAMES[node], info.OwnerId == myTeam ? "your side" : "them"));
                    else if (info.State == POINT_CONTROLLED)
                        ++(info.OwnerId == myTeam ? ourGraves : theirGraves);
                }

                uint32 oursBurned = 0;
                uint32 theirsBurned = 0;
                for (uint8 node = BG_AV_NODES_DUNBALDAR_SOUTH; node < BG_AV_NODES_MAX; ++node)
                {
                    BG_AV_NodeInfo const& info = av->GetAVNodeInfo(node);
                    char const* name = TOWER_NAMES[node - BG_AV_NODES_DUNBALDAR_SOUTH];
                    if (info.State == POINT_DESTROYED)
                        ++(info.TotalOwnerId == myTeam ? oursBurned : theirsBurned);
                    else if (info.State == POINT_ASSAULTED)
                        contested.push_back(Acore::StringFormat("{} is burning {}",
                            info.TotalOwnerId == myTeam ? "they have" : "you have", name));
                }

                std::vector<std::string> clauses;
                clauses.push_back(Acore::StringFormat("you hold {} graveyards to their {}", ourGraves, theirGraves));
                clauses.push_back(Acore::StringFormat("{} of their towers are down and {} of yours",
                    theirsBurned, oursBurned));
                for (std::string const& note : contested)
                    clauses.push_back(note);
                if (!av->IsCaptainAlive(enemyTeam))
                    clauses.push_back("their captain is already dead");
                if (!av->IsCaptainAlive(myTeam))
                    clauses.push_back("your own captain is dead");

                facts.state = Acore::StringFormat("Reinforcements {}-{} of {} (you-them, counting down), {}: {}",
                    av->GetReinforcements(myTeam), av->GetReinforcements(enemyTeam), target,
                    PaceClause(bg), JoinClauses(clauses));
            }

            facts.shorthand = "Places get called by short names - sh stonehearth, ib iceblood, sf snowfall,"
                " tp tower point, iwb icewing bunker, db the Dun Baldar bunkers, fwgy the Frostwolf"
                " graveyard - and the standing calls are \"rush\" for pushing straight at the enemy general,"
                " \"back cap\" for enemies slipping behind to retake a graveyard, and \"def\" for holding a"
                " tower or graveyard that is being taken.";
            return facts;
        }

        MatchFacts IsleOfConquest(Player* bot, Battleground* bg, bool inProgress)
        {
            BattlegroundIC* ic = static_cast<BattlegroundIC*>(bg);
            TeamId myTeam = bot->GetTeamId();
            TeamId enemyTeam = myTeam == TEAM_ALLIANCE ? TEAM_HORDE : TEAM_ALLIANCE;

            MatchFacts facts;
            facts.name = "Isle of Conquest";
            facts.rules = Acore::StringFormat("both sides start with {} reinforcements and bleed them on"
                " deaths and lost objectives - run the enemy to zero, or break into their keep and kill"
                " their commander. The workshop builds demolishers and glaive throwers, the docks catapults"
                " and the hangar the gunship, and the quarry and refinery buff whoever holds them",
                uint32(MAX_REINFORCEMENTS));

            if (inProgress)
            {
                constexpr char const* NODE_NAMES[] =
                {
                    "the Refinery", "the Quarry", "the Docks", "the Hangar", "the Workshop"
                };

                // A node in conflict has not changed hands yet - the side in
                // the state is the one clicking, not the one holding it.
                auto claimant = [](ICNodeState state, bool& capping) -> TeamId
                {
                    capping = state == NODE_STATE_CONFLICT_A || state == NODE_STATE_CONFLICT_H;
                    if (state == NODE_STATE_CONTROLLED_A || state == NODE_STATE_CONFLICT_A)
                        return TEAM_ALLIANCE;
                    if (state == NODE_STATE_CONTROLLED_H || state == NODE_STATE_CONFLICT_H)
                        return TEAM_HORDE;
                    return TEAM_NEUTRAL;
                };

                std::vector<std::string> ours, theirs, unclaimed, contested;
                for (uint8 node = NODE_TYPE_REFINERY; node <= NODE_TYPE_WORKSHOP; ++node)
                {
                    bool capping = false;
                    TeamId owner = claimant(ic->GetICNodePoint(node).nodeState, capping);
                    if (owner == TEAM_NEUTRAL)
                        unclaimed.emplace_back(NODE_NAMES[node]);
                    else if (capping)
                        contested.push_back(Acore::StringFormat("{} is being taken by {}", NODE_NAMES[node],
                            owner == myTeam ? "your side" : "them"));
                    else
                        (owner == myTeam ? ours : theirs).emplace_back(NODE_NAMES[node]);
                }

                // The two keep graveyards read best relative to the bot: what
                // matters is whether the enemy is rezzing inside your keep, or
                // you inside theirs - each side starts holding its own.
                for (uint8 node = NODE_TYPE_GRAVEYARD_A; node <= NODE_TYPE_GRAVEYARD_H; ++node)
                {
                    bool capping = false;
                    TeamId owner = claimant(ic->GetICNodePoint(node).nodeState, capping);
                    if (owner == TEAM_NEUTRAL)
                        continue;

                    bool atOurKeep = (node == NODE_TYPE_GRAVEYARD_A) == (myTeam == TEAM_ALLIANCE);
                    if (atOurKeep && owner != myTeam)
                        contested.push_back(capping ? "they are taking the graveyard at your own keep"
                            : "the graveyard at your own keep is theirs");
                    else if (atOurKeep && capping)
                        contested.push_back("you are retaking the graveyard at your own keep");
                    else if (!atOurKeep && owner == myTeam)
                        contested.push_back(capping ? "you are taking the graveyard at their keep"
                            : "you hold the graveyard at their keep");
                    else if (!atOurKeep && capping)
                        contested.push_back("they are retaking the graveyard at their own keep");
                }

                std::vector<std::string> breached;
                for (uint8 gate = BG_IC_H_FRONT; gate < BG_IC_MAXDOOR; ++gate)
                {
                    if (ic->GetGateState(gate) != BG_IC_GATE_DESTROYED)
                        continue;
                    bool hordeGate = gate <= BG_IC_H_EAST;
                    char const* side = (hordeGate == (myTeam == TEAM_HORDE)) ? "your keep" : "their keep";
                    char const* which = (gate == BG_IC_H_FRONT || gate == BG_IC_A_FRONT) ? "front"
                        : (gate == BG_IC_H_WEST || gate == BG_IC_A_WEST) ? "west" : "east";
                    breached.push_back(Acore::StringFormat("the {} gate of {} is down", which, side));
                }

                std::vector<std::string> clauses;
                clauses.push_back(ours.empty() ? "you hold nothing"
                    : Acore::StringFormat("you hold {}", JoinNames(ours)));
                clauses.push_back(theirs.empty() ? "they hold nothing"
                    : Acore::StringFormat("they hold {}", JoinNames(theirs)));
                if (!unclaimed.empty())
                    clauses.push_back(Acore::StringFormat("{} still unclaimed", JoinNames(unclaimed)));
                for (std::string const& note : contested)
                    clauses.push_back(note);
                for (std::string const& note : breached)
                    clauses.push_back(note);

                facts.state = Acore::StringFormat("Reinforcements {}-{} of {} (you-them, counting down), {}: {}",
                    ic->GetReinforcements(myTeam), ic->GetReinforcements(enemyTeam), uint32(MAX_REINFORCEMENTS),
                    PaceClause(bg), JoinClauses(clauses));
            }

            facts.shorthand = "Teammates call the objectives in shorthand - wsp or shop for the workshop,"
                " docks, hangar, quarry, refinery, gy for a graveyard - so \"inc docks\" means enemies are"
                " closing on the docks, \"demo\" is a demolisher out of the workshop, and \"gates\" or"
                " \"in keep\" means the fight has moved inside a keep.";
            return facts;
        }

        MatchFacts StrandOfTheAncients(Player* bot, Battleground* bg, bool inProgress)
        {
            constexpr char const* GATE_NAMES[] =
            {
                "the Green Emerald gate", "the Yellow Moon gate", "the Blue Sapphire gate",
                "the Red Sun gate", "the Purple Amethyst gate", "the Ancient gate"
            };

            BattlegroundSA* sa = static_cast<BattlegroundSA*>(bg);
            TeamId myTeam = bot->GetTeamId();
            bool attacking = sa->GetAttackerTeamId() == myTeam;
            BG_SA_Status roundStatus = sa->GetRoundStatus();
            bool secondRound = roundStatus == BG_SA_SECOND_WARMUP || roundStatus == BG_SA_ROUND_TWO
                || roundStatus == BG_SA_BONUS_ROUND;

            MatchFacts facts;
            facts.name = "Strand of the Ancients";
            facts.rules = "each side gets one turn attacking the beach - blow through the gates, reach the"
                " Titan relic in the last chamber - and whoever does it faster wins";

            // Between the rounds everyone is being shipped back out to the
            // boats: sides are known, the beach is not yet in play.
            if (inProgress && (roundStatus == BG_SA_WARMUP || roundStatus == BG_SA_SECOND_WARMUP))
            {
                facts.state = Acore::StringFormat("Round {} is about to start and you are {} this time.",
                    secondRound ? "two" : "one", attacking ? "attacking" : "defending");
            }
            else if (inProgress)
            {
                std::vector<std::string> down, damaged;
                for (uint8 gate = BG_SA_GREEN_GATE; gate <= BG_SA_ANCIENT_GATE; ++gate)
                {
                    if (sa->GetGateState(gate) == BG_SA_GATE_DESTROYED)
                        down.emplace_back(GATE_NAMES[gate]);
                    else if (sa->GetGateState(gate) == BG_SA_GATE_DAMAGED)
                        damaged.emplace_back(GATE_NAMES[gate]);
                }

                std::vector<std::string> graveyards;
                if (sa->GetGraveyardOwner(BG_SA_LEFT_CAPTURABLE_GY) == myTeam)
                    graveyards.emplace_back("the left");
                if (sa->GetGraveyardOwner(BG_SA_CENTRAL_CAPTURABLE_GY) == myTeam)
                    graveyards.emplace_back("the central");
                if (sa->GetGraveyardOwner(BG_SA_RIGHT_CAPTURABLE_GY) == myTeam)
                    graveyards.emplace_back("the right");

                std::vector<std::string> clauses;
                clauses.push_back(down.empty() ? "every gate still stands"
                    : Acore::StringFormat("{} down", JoinNames(down)));
                if (!damaged.empty())
                    clauses.push_back(Acore::StringFormat("{} taking damage", JoinNames(damaged)));
                clauses.push_back(graveyards.empty() ? "you hold none of the beach graveyards"
                    : Acore::StringFormat("you hold {} beach graveyard{}", JoinNames(graveyards),
                        graveyards.size() > 1 ? "s" : ""));

                int64 remaining = std::max<int64>(sa->GetRoundTimeRemaining().count(), 0);
                std::string clock = Acore::StringFormat("{}:{:02}", remaining / 60000, (remaining / 1000) % 60);

                facts.state = Acore::StringFormat("Round {}, you are {}, {} on the clock{}: {}",
                    secondRound ? "two" : "one",
                    attacking ? "attacking" : "defending", clock,
                    secondRound ? " - which is the time the other side needed, so beating it wins the match"
                        : "",
                    JoinClauses(clauses));
            }

            facts.shorthand = "Teammates call the gates by colour - green, blue, red, purple, yellow, then"
                " the Ancient gate at the top - and \"demo\" is a demolisher, \"bomb\" or \"seaforium\" the"
                " charges that blow a gate, \"relic\" the last room.";
            return facts;
        }
    }

    std::string Describe(Player* bot)
    {
        Battleground* bg = bot->GetBattleground();
        if (!bg || bg->isArena())
            return "";

        // Before the gates open a player still knows which map they drew and
        // how it is won; after the match ends the scoreboard is frozen and
        // nothing is worth calling.
        BattlegroundStatus status = bg->GetStatus();
        bool inProgress = status == STATUS_IN_PROGRESS;
        if (!inProgress && status != STATUS_WAIT_JOIN)
            return "";

        BattlegroundTypeId bgType = bg->GetBgTypeID();
        if (bgType == BATTLEGROUND_RB)
            bgType = bg->GetBgTypeID(true);

        MatchFacts facts;
        switch (bgType)
        {
            case BATTLEGROUND_WS:
                facts = WarsongGulch(bot, bg, inProgress);
                break;
            case BATTLEGROUND_AB:
                facts = ArathiBasin(bot, bg, inProgress);
                break;
            case BATTLEGROUND_EY:
                facts = EyeOfTheStorm(bot, bg, inProgress);
                break;
            case BATTLEGROUND_AV:
                facts = AlteracValley(bot, bg, inProgress);
                break;
            case BATTLEGROUND_IC:
                facts = IsleOfConquest(bot, bg, inProgress);
                break;
            case BATTLEGROUND_SA:
                facts = StrandOfTheAncients(bot, bg, inProgress);
                break;
            default:
                return "";
        }

        if (!inProgress)
            return Acore::StringFormat(" You are in {}, waiting in the starting area for the gates to open:"
                " {}. Nothing has happened yet. {}",
                facts.name, facts.rules, facts.shorthand);

        return Acore::StringFormat(" You are mid-match in {}: {}. {} {}",
            facts.name, facts.rules, facts.state, facts.shorthand);
    }
}
