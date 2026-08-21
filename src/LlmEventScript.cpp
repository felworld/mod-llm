/*
 * This file is part of mod-llm for Felworld. Released under the MIT license
 * (see the LICENSE file at the module root).
 */

#include "BotSelector.h"
#include "ChatHelper.h"
#include "Creature.h"
#include "DBCEnums.h"
#include "DBCStructure.h"
#include "Group.h"
#include "HistoryStore.h"
#include "Item.h"
#include "ItemTemplate.h"
#include "LevelPerception.h"
#include "LlmConfig.h"
#include "LlmDispatch.h"
#include "Map.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "Playerbots.h"
#include "QuestDef.h"
#include "Random.h"
#include "ScriptMgr.h"
#include "SharedDefines.h"
#include "StringFormat.h"
#include "WpvpDefense.h"

#include <chrono>
#include <functional>
#include <mutex>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace ModLlm
{
    // Game-event triggers: nearby bots may comment on kills, deaths, level-ups,
    // quests, duels, achievements, and notable loot.
    //
    // Threading invariant: unlike the chat hooks (world thread), these hooks
    // can fire on map-update threads. Candidate selection is therefore
    // restricted to players on the source's own map (BotSelector::SelectNearby)
    // and the cooldown map is mutex-guarded.
    class LlmEventScript : public PlayerScript
    {
    public:
        LlmEventScript() : PlayerScript("LlmEventScript", {
            PLAYERHOOK_ON_CREATURE_KILL,
            PLAYERHOOK_ON_PVP_KILL,
            PLAYERHOOK_ON_PLAYER_JUST_DIED,
            PLAYERHOOK_ON_PLAYER_COMPLETE_QUEST,
            PLAYERHOOK_ON_LEVEL_CHANGED,
            PLAYERHOOK_ON_DUEL_REQUEST,
            PLAYERHOOK_ON_DUEL_END,
            PLAYERHOOK_ON_ACHI_COMPLETE,
            PLAYERHOOK_ON_STORE_NEW_ITEM,
            PLAYERHOOK_ON_GROUP_ROLL_REWARD_ITEM
        }) { }

        void OnPlayerCreatureKill(Player* killer, Creature* killed) override
        {
            // Routine grind kills are the group's business only: players
            // don't narrate a stranger's mob kill, and a passerby's "gg"
            // mid-pull - the killer often still swarmed and hurt - reads as
            // nonsense to someone fighting for their life. Strangers only
            // remark on a kill that would turn a head: a rare, an elite, or
            // a mob well above the killer's level. The killer's own group
            // (the killer included) keeps the usual dice - kill banter is
            // party chat's bread and butter.
            char const* rankPhrase = nullptr;
            switch (killed->GetCreatureTemplate()->rank)
            {
                case CREATURE_ELITE_RARE:
                    rankPhrase = "a rare";
                    break;
                case CREATURE_ELITE_ELITE:
                    rankPhrase = "an elite";
                    break;
                case CREATURE_ELITE_RAREELITE:
                    rankPhrase = "a rare elite";
                    break;
                case CREATURE_ELITE_WORLDBOSS:
                    rankPhrase = "a world boss";
                    break;
                default:
                    break;
            }

            // +4 and up is the orange-to-red con a player would see on the
            // mob - the gap where a kill starts looking like a feat.
            int32 levelGap = int32(killed->GetLevel()) - int32(killer->GetLevel());
            bool notable = rankPhrase || levelGap >= 4;

            ObjectGuid killerGuid = killer->GetGUID();
            Group* killerGroup = killer->GetGroup();
            std::string killerName = killer->GetName();
            std::string killedName = killed->GetName();

            DispatchEvent(killer, "creature_kill", sLlmConfig->eventChanceKill,
                [killerGuid, killerGroup, killerName, killedName, rankPhrase, levelGap, notable](Player* bot)
                {
                    if (!notable && (!killerGroup || bot->GetGroup() != killerGroup))
                        return std::string();

                    bool self = bot->GetGUID() == killerGuid;

                    // Name what made the kill notable, so the model's
                    // register matches - an elite down is not a boar down.
                    std::string what = killedName;
                    if (rankPhrase)
                        what += Acore::StringFormat(", {}", rankPhrase);
                    else if (notable)
                        what += Acore::StringFormat(", {} levels above {}", levelGap, self ? "you" : "them");

                    return self ? Acore::StringFormat("you killed {}", what)
                                : Acore::StringFormat("{} killed {}", killerName, what);
                },
                nullptr, /*narrate*/ false);
        }

        void OnPlayerPVPKill(Player* killer, Player* killed) override
        {
            ObjectGuid killerGuid = killer->GetGUID();
            ObjectGuid killedGuid = killed->GetGUID();
            std::string killerName = killer->GetName();
            std::string killedName = killed->GetName();
            TeamId killerTeam = killer->GetTeamId();
            DispatchEvent(killer, "pvp_kill", sLlmConfig->eventChancePvpKill,
                [killerGuid, killedGuid, killerName, killedName, killerTeam](Player* bot)
                {
                    if (bot->GetGUID() == killerGuid)
                        return Acore::StringFormat("you killed the enemy {} in PvP", killedName);
                    if (bot->GetGUID() == killedGuid)
                        return Acore::StringFormat("the enemy {} killed you in PvP", killerName);
                    // A faction-blind "X killed Y" reads as a threat either
                    // way, and a bot would warn its own side about an ally
                    // clearing enemy gankers. Names carry no faction, so the
                    // sides are spelled out relative to the reacting bot.
                    if (bot->GetTeamId() == killerTeam)
                        return Acore::StringFormat("your ally {} killed the enemy {} in PvP",
                            killerName, killedName);
                    return Acore::StringFormat("the enemy {} killed your ally {} in PvP",
                        killerName, killedName);
                });
        }

        void OnPlayerJustDied(Player* player) override
        {
            DispatchEvent(player, "death", sLlmConfig->eventChanceDeath,
                ActorAware(player->GetGUID(), "you just died",
                    Acore::StringFormat("{} just died", player->GetName())));
        }

        // Fires on turn-in (Player::RewardQuest). A turn-in shows nothing on
        // a bystander's screen - no flash, no announcement - so a stranger's
        // "grats on finishing that" reads as mind-reading. Only the quester
        // and their group, who share the quest and the grind, see the event
        // at all; strangers get neither the comment nor the narration.
        void OnPlayerCompleteQuest(Player* player, Quest const* quest) override
        {
            ObjectGuid questerGuid = player->GetGUID();
            Group* questerGroup = player->GetGroup();
            std::string selfDescription = Acore::StringFormat("you completed the quest \"{}\"", quest->GetTitle());
            std::string otherDescription = Acore::StringFormat("{} completed the quest \"{}\"",
                player->GetName(), quest->GetTitle());

            DispatchEvent(player, "quest_complete", sLlmConfig->eventChanceQuestComplete,
                [questerGuid, questerGroup, selfDescription = std::move(selfDescription),
                    otherDescription = std::move(otherDescription)](Player* bot)
                {
                    if (bot->GetGUID() == questerGuid)
                        return selfDescription;
                    if (questerGroup && bot->GetGroup() == questerGroup)
                        return otherDescription;
                    return std::string();
                });
        }

        void OnPlayerLevelChanged(Player* player, uint8 oldLevel) override
        {
            if (player->GetLevel() <= oldLevel)
                return;
            DispatchEvent(player, "level_up", sLlmConfig->eventChanceLevelUp,
                ActorAware(player->GetGUID(),
                    Acore::StringFormat("you reached level {}", player->GetLevel()),
                    Acore::StringFormat("{} reached level {}", player->GetName(), player->GetLevel())));
        }

        // Duels are the duelists' story: at gate duel spots a spoken comment
        // per challenge and per outcome - times two picked bots, times the
        // replies each line invites - drowned the area in "gl"/"gg" chatter
        // (felworld/mod-llm#22). Bystanders now only see the narration; the
        // one reaction that carries weight, the "gg" at the end, comes from
        // the participants themselves.
        void OnPlayerDuelRequest(Player* target, Player* challenger) override
        {
            ObjectGuid targetGuid = target->GetGUID();
            ObjectGuid challengerGuid = challenger->GetGUID();
            std::string targetName = target->GetName();
            std::string challengerName = challenger->GetName();
            DispatchEvent(target, "duel_request", 0,
                [targetGuid, challengerGuid, targetName, challengerName](Player* bot)
                {
                    if (bot->GetGUID() == targetGuid)
                        return Acore::StringFormat("{} challenged you to a duel", challengerName);
                    if (bot->GetGUID() == challengerGuid)
                        return Acore::StringFormat("you challenged {} to a duel", targetName);
                    return Acore::StringFormat("{} challenged {} to a duel", challengerName, targetName);
                },
                challenger);
        }

        void OnPlayerDuelEnd(Player* winner, Player* loser, DuelCompleteType type) override
        {
            if (type != DUEL_WON)
                return;

            ObjectGuid winnerGuid = winner->GetGUID();
            ObjectGuid loserGuid = loser->GetGUID();
            std::string winnerName = winner->GetName();
            std::string loserName = loser->GetName();

            // The duelists themselves are described (and dispatched) by
            // DispatchDuelist below; an empty description keeps this loop
            // from narrating their own duel at them in the third person.
            DispatchEvent(winner, "duel_end", 0,
                [winnerGuid, loserGuid, winnerName, loserName](Player* bot)
                {
                    if (bot->GetGUID() == winnerGuid || bot->GetGUID() == loserGuid)
                        return std::string();
                    return Acore::StringFormat("{} won a duel against {}", winnerName, loserName);
                });

            DispatchDuelist(winner, loser, Acore::StringFormat("you won a duel against {}", loserName));
            DispatchDuelist(loser, winner, Acore::StringFormat("you lost a duel against {}", winnerName));
        }

        // An achievement is the one event with an audience past line of
        // sight: WotLK announces it to everyone standing around and to the
        // whole guild. The vicinity half is the noisy one - nobody has ever
        // turned to a stranger over their "Expert First Aid"
        // (felworld/mod-llm#50) - so only the audiences that would actually
        // talk about it hear one here: the achiever's party or raid (never a
        // battleground group, where the match is the only subject) and its
        // guild. Both reach across maps while this hook runs on a map
        // thread, so the audience is walked on the world thread instead.
        void OnPlayerAchievementComplete(Player* player, AchievementEntry const* achievement) override
        {
            if (!sLlmConfig->IsEnabled() || !sLlmConfig->eventEnabled)
                return;
            if (!IsNotableAchievement(achievement))
                return;

            char const* name = achievement->name[0];
            Dispatch::RunDelayed([this, achieverGuid = player->GetGUID(), achieverName = player->GetName(),
                title = std::string(name ? name : "?")]
                {
                    DispatchAchievement(achieverGuid, achieverName, title);
                }, 1);
        }

        void OnPlayerStoreNewItem(Player* player, Item* item, uint32 /*count*/) override
        {
            ItemTemplate const* proto = item->GetTemplate();
            if (!proto || proto->Quality < sLlmConfig->eventLootMinQuality)
                return;

            // Under a rolling loot method, an item at or above the group's
            // threshold reached the winner's bags through a roll - the roll
            // hook below tells that story ("you won the need roll on ..."),
            // so the generic loot comment stays out of its way.
            if (Group* group = player->GetGroup())
                if ((group->GetLootMethod() == GROUP_LOOT || group->GetLootMethod() == NEED_BEFORE_GREED)
                    && proto->Quality >= static_cast<uint32>(group->GetLootThreshold()))
                    return;

            DispatchEvent(player, "loot", sLlmConfig->eventChanceLoot,
                ActorAware(player->GetGUID(),
                    Acore::StringFormat("you obtained [{}]", proto->Name1),
                    Acore::StringFormat("{} obtained [{}]", player->GetName(), proto->Name1)));
        }

        // Group loot roll decided: the winner may gloat, losing rollers may
        // grumble (or congratulate - the model's call), and every bot that
        // saw the roll frames learns the outcome. Greed rolls are routine
        // and stay narration-only; Need rolls carry the drama.
        void OnPlayerGroupRollRewardItem(Player* winner, Item* item, uint32 /*count*/, RollVote voteType,
            Roll* roll) override
        {
            if (!sLlmConfig->IsEnabled() || !sLlmConfig->eventEnabled)
                return;

            Group* group = winner->GetGroup();
            ItemTemplate const* proto = item->GetTemplate();
            if (!group || group->isBGGroup() || group->isBFGroup() || !proto)
                return;

            char const* rollWord = voteType == NEED ? "need" : "greed";
            ObjectGuid winnerGuid = winner->GetGUID();
            std::string winnerName = winner->GetName();

            bool reactWorthy = voteType == NEED && proto->Quality >= sLlmConfig->eventLootMinQuality
                && BotSelector::GroupHasRealPlayer(group);

            uint32 dispatched = 0;
            for (auto const& [voterGuid, vote] : roll->playerVote)
            {
                Player* bot = ObjectAccessor::FindPlayer(voterGuid);
                if (!bot || IsRealPlayer(bot))
                    continue;

                bool won = voterGuid == winnerGuid;
                bool lost = !won && vote == voteType; // rolled the winning way, dice said no

                std::string description = won
                    ? Acore::StringFormat("you won the {} roll on [{}]", rollWord, proto->Name1)
                    : lost
                        ? Acore::StringFormat("you lost the {} roll on [{}] to {}", rollWord, proto->Name1,
                            winnerName)
                        : Acore::StringFormat("{} won the {} roll on [{}]", winnerName, rollWord, proto->Name1);

                // Every participant watched the roll frames resolve on
                // screen, so the outcome lands in each bot's overheard
                // transcript whether or not the dice pick it to react.
                sLlmHistoryStore->AddOverheardLine(bot->GetGUID(), "",
                    Acore::StringFormat("({})", description));

                if (!reactWorthy || (!won && !lost))
                    continue;
                if (dispatched >= sLlmConfig->eventMaxBotsPerEvent)
                    continue;
                if (urand(0, 99) >= (won ? sLlmConfig->eventChanceRollWon : sLlmConfig->eventChanceRollLost))
                    continue;
                if (IsOnCooldown(bot->GetGUID()))
                    continue;

                TriggerContext trigger;
                trigger.kind = TRIGGER_GAME_EVENT;
                trigger.eventType = won ? "roll_won" : "roll_lost";
                trigger.message = description;
                trigger.chatType = group->isRaidGroup() ? CHAT_MSG_RAID : CHAT_MSG_PARTY;
                trigger.roomKey = Acore::StringFormat("group:{}", group->GetGUID().GetCounter());

                if (!Dispatch::Submit(bot, won ? nullptr : winner, std::move(trigger)))
                    continue;

                StartCooldown(bot->GetGUID());
                ++dispatched;
            }
        }

    private:
        // Description resolved per reacting bot: participants are addressed
        // as "you", bystanders read names. A small model reliably binds
        // "you killed X" where it may not recognize its own name in a
        // third-person line - and then congratulates itself on its own kill.
        using EventDescriber = std::function<std::string(Player* bot)>;

        // Only feats that carry their own story - a ding, a rare drop - are
        // worth retelling to a whole zone or battleground team. Play-by-play
        // (mob pulls, deaths, duels, PvP kills) is invisible to readers who
        // are not standing there: the prompt asks the model to retell or stay
        // silent, but small models still produce "nice pulls", so the gate is
        // enforced here and those comments stay in local /say. Achievements
        // never reach this path - their audience is the achiever's group and
        // guild, nothing wider (see OnPlayerAchievementComplete).
        static bool IsBroadcastWorthy(char const* eventType)
        {
            std::string_view type(eventType);
            return type == "level_up" || type == "loot";
        }

        // Rare enough to be worth a word, in the terms the DBC gives us:
        // realm firsts by flag; Feats of Strength, which award no points at
        // all precisely because they cannot be farmed (statistics also score
        // zero, but they are counters that never complete and so never reach
        // this hook); and anything worth MinPoints or more, which is where
        // the metas and the hard content sit. What that leaves out is the
        // routine ten-pointers - a profession rank, a zone's quests done, a
        // dungeon cleared - which is the whole point.
        static bool IsNotableAchievement(AchievementEntry const* achievement)
        {
            // Tracking entries are not sent to anyone's client at all.
            if (achievement->flags & ACHIEVEMENT_FLAG_HIDDEN)
                return false;
            if (achievement->flags & (ACHIEVEMENT_FLAG_REALM_FIRST_KILL | ACHIEVEMENT_FLAG_REALM_FIRST_REACH))
                return true;
            if (!achievement->points)
                return true;
            return achievement->points >= sLlmConfig->eventAchievementMinPoints;
        }

        // Describer for the common single-actor event: the actor hears
        // selfDescription, everyone else hears otherDescription.
        static EventDescriber ActorAware(ObjectGuid actorGuid, std::string selfDescription,
            std::string otherDescription)
        {
            return [actorGuid, selfDescription = std::move(selfDescription),
                otherDescription = std::move(otherDescription)](Player* bot)
            {
                return bot->GetGUID() == actorGuid ? selfDescription : otherDescription;
            };
        }

        // The world-thread half of OnPlayerAchievementComplete: gather the
        // achiever's party/raid and guild, tell each bot in them what
        // happened, and let a couple of them react. Neither audience is a
        // spatial query - a guild is spread over every map - so this walks
        // the online roster once rather than the bots standing nearby.
        void DispatchAchievement(ObjectGuid achieverGuid, std::string const& achieverName,
            std::string const& title)
        {
            Player* achiever = ObjectAccessor::FindPlayer(achieverGuid);
            if (!achiever || !achiever->IsInWorld())
                return;

            Group* group = achiever->GetGroup();
            if (group && (group->isBGGroup() || group->isBFGroup()))
                group = nullptr;

            uint32 guildId = achiever->GetGuildId();
            if (!group && !guildId)
                return;

            struct Listener
            {
                Player* bot;
                bool inGroup;
                bool inGuild;
            };

            bool groupHasHuman = false;
            bool guildHasHuman = false;
            std::vector<Listener> listeners;
            for (auto const& [guid, player] : ObjectAccessor::GetPlayers())
            {
                if (!player->IsInWorld())
                    continue;

                bool inGroup = group && player->GetGroup() == group;
                bool inGuild = guildId && player->GetGuildId() == guildId;
                if (!inGroup && !inGuild)
                    continue;

                if (IsRealPlayer(player))
                {
                    groupHasHuman = groupHasHuman || inGroup;
                    guildHasHuman = guildHasHuman || inGuild;
                    continue;
                }

                PlayerbotAI* botAI = GET_PLAYERBOT_AI(player);
                if (!botAI || !botAI->IsBotAI())
                    continue;
                if (sLlmConfig->skipInCombat && player->IsInCombat())
                    continue;

                listeners.push_back({ player, inGroup, inGuild });
            }

            uint32 dispatched = 0;
            for (Listener const& listener : listeners)
            {
                // Bots congratulating bots in an empty room is nobody's
                // conversation: each audience needs a human reading it.
                bool viaGroup = listener.inGroup && groupHasHuman;
                bool viaGuild = listener.inGuild && guildHasHuman;
                if (!viaGroup && !viaGuild)
                    continue;

                Player* bot = listener.bot;
                bool self = bot->GetGUID() == achieverGuid;

                // A guildmate's feat is heard about, not witnessed - the
                // announcement can come from the other end of the world - so
                // the relationship is named, or the model wonders how it
                // knows.
                std::string description = self
                    ? Acore::StringFormat("you earned the achievement \"{}\"", title)
                    : Acore::StringFormat("{}{} earned the achievement \"{}\"",
                        viaGroup ? "" : "your guildmate ", achieverName, title);

                // The announcement landed on every one of their screens
                // whether or not the dice pick anyone to say something.
                sLlmHistoryStore->AddOverheardLine(bot->GetGUID(), "",
                    Acore::StringFormat("({})", description));

                if (dispatched >= sLlmConfig->eventMaxBotsPerEvent)
                    continue;
                if (urand(0, 99) >= sLlmConfig->eventChanceAchievement)
                    continue;
                if (IsOnCooldown(bot->GetGUID()))
                    continue;

                TriggerContext trigger;
                trigger.kind = TRIGGER_GAME_EVENT;
                trigger.eventType = "achievement";
                trigger.message = description;

                // Answer where the announcement itself appeared: party or
                // raid chat for a groupmate's feat, guild chat otherwise.
                if (viaGroup)
                {
                    trigger.chatType = group->isRaidGroup() ? CHAT_MSG_RAID : CHAT_MSG_PARTY;
                    trigger.roomKey = Acore::StringFormat("group:{}", group->GetGUID().GetCounter());
                }
                else
                {
                    trigger.chatType = CHAT_MSG_GUILD;
                    trigger.roomKey = Acore::StringFormat("guild:{}", guildId);
                }

                if (!Dispatch::Submit(bot, self ? nullptr : achiever, std::move(trigger)))
                    continue;

                StartCooldown(bot->GetGUID());
                ++dispatched;
            }
        }

        void DispatchEvent(Player* source, char const* eventType, uint32 chance, std::string description,
            Player* actorOverride = nullptr, bool narrate = true)
        {
            DispatchEvent(source, eventType, chance,
                [description = std::move(description)](Player* /*bot*/) { return description; },
                actorOverride, narrate);
        }

        // The duelists' own reactions, dispatched directly rather than
        // through SelectNearby: OnPlayerDuelEnd fires before DuelComplete's
        // AttackStop, so both are still flagged in combat and the
        // skip-in-combat filter would drop exactly the two bots whose story
        // this is. Submit() itself has no combat gate, and the reply is
        // "typed" out over a few seconds anyway - by delivery the dust has
        // settled.
        void DispatchDuelist(Player* bot, Player* opponent, std::string description)
        {
            if (!sLlmConfig->IsEnabled() || !sLlmConfig->eventEnabled)
                return;
            if (IsRealPlayer(bot))
                return;

            // The duelist remembers its own duel whether or not it speaks.
            sLlmHistoryStore->AddOverheardLine(bot->GetGUID(), "",
                Acore::StringFormat("({})", description));

            if (urand(0, 99) >= sLlmConfig->eventChanceDuel)
                return;
            if (IsOnCooldown(bot->GetGUID()))
                return;
            if (!BotSelector::HasRealPlayerNearby(bot, sLlmConfig->sayDistance))
                return;

            TriggerContext trigger;
            trigger.kind = TRIGGER_GAME_EVENT;
            trigger.eventType = "duel_end";
            trigger.message = std::move(description);

            if (!Dispatch::Submit(bot, opponent, std::move(trigger)))
                return;

            StartCooldown(bot->GetGUID());
        }

        void DispatchEvent(Player* source, char const* eventType, uint32 chance, EventDescriber describe,
            Player* actorOverride = nullptr, bool narrate = true)
        {
            if (!sLlmConfig->IsEnabled() || !sLlmConfig->eventEnabled)
                return;

            // chance 0 = narration only: every nearby bot still sees the
            // event, nobody is picked to comment on it.
            if (!chance && !narrate)
                return;

            Player* actor = actorOverride ? actorOverride : source;

            // Include the source itself: a bot may react to its own level-up.
            std::vector<Player*> bots = BotSelector::SelectNearby(source, sLlmConfig->eventBotDistance,
                16, true);

            uint32 dispatched = 0;
            for (Player* bot : bots)
            {
                std::string description = describe(bot);

                // An empty description means this bot is handled outside the
                // loop (e.g. the duelists themselves) - nothing to see or say.
                if (description.empty())
                    continue;

                // Seeing and reacting are different things: the event lands in
                // every nearby bot's overheard transcript whether or not the
                // dice pick it to react, so a later trigger - a bow right
                // after a duel - still knows what it is about. Events are
                // visual, so narration ignores the faction line. Mob kills
                // are exempt: grinding would flood the transcript with them.
                if (narrate)
                    sLlmHistoryStore->AddOverheardLine(bot->GetGUID(), "",
                        Acore::StringFormat("({})", description));

                if (dispatched >= sLlmConfig->eventMaxBotsPerEvent)
                    continue;
                if (urand(0, 99) >= chance)
                    continue;
                if (IsOnCooldown(bot->GetGUID()))
                    continue;

                TriggerContext trigger;
                trigger.kind = TRIGGER_GAME_EVENT;
                trigger.eventType = eventType;
                trigger.message = description;

                // An enemy's deed rarely draws words - there is no shared
                // language. The occasional exception is deliberate: shouted
                // cross-faction gibberish is a proud tradition.
                if (bot != actor && !BotSelector::CanUnderstand(bot, actor))
                {
                    if (urand(0, 99) >= sLlmConfig->crossFactionChatChance)
                        continue;
                    trigger.crossFaction = true;
                    trigger.crossFactionChatOk = true;
                }

                // A comment about a groupmate (or the bot's own feat while
                // grouped) belongs in group chat. Otherwise some comments go
                // to the wide audience - the battleground team in a match,
                // the zone's General channel outside one - which needs the
                // world thread to resolve, so the wish rides along and the
                // delayed dispatch binds it (falling back to /say when the
                // bot has no such audience or no human reads it). The rest is
                // said aloud, which is only worth doing with a human in
                // earshot.
                Group* group = bot->GetGroup();
                if (group && actor->GetGroup() == group && !group->isBGGroup() && !group->isBFGroup())
                {
                    if (!BotSelector::GroupHasRealPlayer(group))
                        continue;
                    trigger.chatType = group->isRaidGroup() ? CHAT_MSG_RAID : CHAT_MSG_PARTY;
                    trigger.roomKey = Acore::StringFormat("group:{}", group->GetGUID().GetCounter());
                }
                else if (IsBroadcastWorthy(eventType) && urand(0, 99) < sLlmConfig->eventChannelChance)
                    trigger.wantAmbientChannel = true;
                else if (!BotSelector::HasRealPlayerNearby(bot, sLlmConfig->sayDistance))
                    continue;

                if (trigger.wantAmbientChannel)
                    Dispatch::SubmitDelayed(bot, actor != bot ? actor : nullptr, std::move(trigger), 1);
                else if (!Dispatch::Submit(bot, actor != bot ? actor : nullptr, std::move(trigger)))
                    continue;

                StartCooldown(bot->GetGUID());
                ++dispatched;
            }
        }

        bool IsOnCooldown(ObjectGuid botGuid)
        {
            std::lock_guard<std::mutex> lock(_cooldownMutex);
            auto it = _cooldowns.find(botGuid.GetRawValue());
            return it != _cooldowns.end()
                && std::chrono::steady_clock::now() - it->second
                    < std::chrono::seconds(sLlmConfig->eventCooldownSeconds);
        }

        void StartCooldown(ObjectGuid botGuid)
        {
            std::lock_guard<std::mutex> lock(_cooldownMutex);
            _cooldowns[botGuid.GetRawValue()] = std::chrono::steady_clock::now();
        }

        std::mutex _cooldownMutex;
        std::unordered_map<uint64, std::chrono::steady_clock::time_point> _cooldowns;
    };

    // A bot thanks whoever heals it - the verbal half of the reaction
    // (mod-playerbots adds the /thank emote and buff-back). Fires only for
    // the healed bot itself, not bystanders: gratitude is personal.
    //
    // Same threading rules as LlmEventScript: OnHeal runs on map-update
    // threads, healer and receiver share a map, and the cooldown map is
    // mutex-guarded.
    class LlmHealedScript : public UnitScript
    {
    public:
        LlmHealedScript() : UnitScript("LlmHealedScript", true, {
            UNITHOOK_ON_HEAL
        }) { }

        void OnHeal(Unit* healerUnit, Unit* receiverUnit, uint32& gain) override
        {
            if (!sLlmConfig->IsEnabled() || !sLlmConfig->eventEnabled || !sLlmConfig->eventChanceHealed || !gain)
                return;
            if (!healerUnit || !receiverUnit || healerUnit == receiverUnit)
                return;

            Player* healer = healerUnit->ToPlayer();
            Player* bot = receiverUnit->ToPlayer();
            if (!healer || !bot || IsRealPlayer(bot))
                return;

            if (urand(0, 99) >= sLlmConfig->eventChanceHealed)
                return;
            if (!BotSelector::CanUnderstand(bot, healer))
                return;
            if (IsOnCooldown(bot->GetGUID()))
                return;

            // A groupmate's heal is routine - thanking the party healer for
            // every splash would be absurd. Only a stranger's kindness draws
            // thanks, said aloud, which is only worth doing with a human in
            // earshot.
            Group* group = bot->GetGroup();
            if (group && healer->GetGroup() == group)
                return;
            if (!BotSelector::HasRealPlayerNearby(bot, sLlmConfig->sayDistance))
                return;

            TriggerContext trigger;
            trigger.kind = TRIGGER_GAME_EVENT;
            trigger.eventType = "healed";
            trigger.message = Acore::StringFormat("{} healed {}", healer->GetName(), bot->GetName());

            if (!Dispatch::Submit(bot, healer, std::move(trigger)))
                return;

            StartCooldown(bot->GetGUID());
        }

    private:
        bool IsOnCooldown(ObjectGuid botGuid)
        {
            std::lock_guard<std::mutex> lock(_cooldownMutex);
            auto it = _cooldowns.find(botGuid.GetRawValue());
            return it != _cooldowns.end()
                && std::chrono::steady_clock::now() - it->second
                    < std::chrono::seconds(sLlmConfig->eventCooldownSeconds);
        }

        void StartCooldown(ObjectGuid botGuid)
        {
            std::lock_guard<std::mutex> lock(_cooldownMutex);
            _cooldowns[botGuid.GetRawValue()] = std::chrono::steady_clock::now();
        }

        std::mutex _cooldownMutex;
        std::unordered_map<uint64, std::chrono::steady_clock::time_point> _cooldowns;
    };

    // A bot greets its new party or raid when it joins one - the LLM
    // replacement for playerbots' canned "Hello" whisper on invite accept
    // (which we keep disabled via AiPlayerbot.EnableGreet = 0).
    //
    // Bots joining fires this hook from the bot's AI update, which runs on
    // map-update threads: only the joining bot (on this thread's map) is
    // touched here; everyone else is read through the group's member slots.
    class LlmGroupScript : public GroupScript
    {
    public:
        LlmGroupScript() : GroupScript("LlmGroupScript", {
            GROUPHOOK_ON_ADD_MEMBER
        }) { }

        void OnAddMember(Group* group, ObjectGuid guid) override
        {
            if (!sLlmConfig->IsEnabled() || !sLlmConfig->eventEnabled)
                return;
            if (group->isBGGroup() || group->isBFGroup())
                return;

            // The leader is "added" when the group is created; nobody to greet.
            if (guid == group->GetLeaderGUID())
                return;

            Player* bot = ObjectAccessor::FindPlayer(guid);
            if (!bot || IsRealPlayer(bot))
                return;
            if (urand(0, 99) >= sLlmConfig->eventChanceGroupJoin)
                return;

            // Don't greet into a group of nothing but bots.
            if (!BotSelector::GroupHasRealPlayer(group))
                return;

            bool raid = group->isRaidGroup();

            TriggerContext trigger;
            trigger.kind = TRIGGER_GAME_EVENT;
            trigger.eventType = "group_join";
            trigger.chatType = raid ? CHAT_MSG_RAID : CHAT_MSG_PARTY;
            trigger.roomKey = Acore::StringFormat("group:{}", group->GetGUID().GetCounter());
            trigger.message = Acore::StringFormat("{} just joined {}'s {}",
                bot->GetName(), group->GetLeaderName(), raid ? "raid" : "party");

            // Leader guid/name come from group data rather than the leader's
            // Player object (who may be updating on another map thread); the
            // actor is re-resolved on the world thread when the trigger fires.
            trigger.actorGuid = group->GetLeaderGUID();
            trigger.actorName = group->GetLeaderName();

            Dispatch::SubmitDelayed(bot, nullptr, std::move(trigger), urand(1500, 4000));
        }
    };

    // The LLM replacement for playerbots' prebaked defense-callout lines
    // (which llm mode disables via AiPlayerbot.WpvpCallouts = 0): playerbots
    // always fires this notification when a callout or escalation is
    // claimed, and the claiming bot raises the alarm in its own words. The
    // defense board and travel responses run in playerbots regardless - only
    // the speech goes through the model.
    //
    // Fires on the speaker's map-update thread: everything is copied into
    // the trigger and the LLM work is queued; only the speaker's own map is
    // scanned for a human audience.
    void OnWpvpCallout(WpvpCalloutNotification const& notification)
    {
        if (!sLlmConfig->IsEnabled() || !sLlmConfig->eventEnabled)
            return;

        bool escalation = notification.kind == WpvpCalloutKind::Escalation;
        uint32 chance = escalation ? sLlmConfig->eventChanceDefenseEscalation
                                   : sLlmConfig->eventChanceDefenseCallout;
        if (!chance || urand(0, 99) >= chance)
            return;

        Player* bot = notification.speaker;
        if (!bot || IsRealPlayer(bot))
            return;

        // LocalDefense only reaches its own zone: without a human there, the
        // words have no audience (responder bots react to the defense board,
        // not the text). WorldDefense is faction-global and escalations are
        // rare, so they are always worth saying.
        if (!escalation)
        {
            Map* map = bot->FindMap();
            if (!map)
                return;

            bool humanInZone = false;
            for (MapReference const& ref : map->GetPlayers())
            {
                Player* player = ref.GetSource();
                if (player && IsRealPlayer(player) && player->GetZoneId() == notification.zoneId)
                {
                    humanInZone = true;
                    break;
                }
            }
            if (!humanInZone)
                return;
        }

        std::string race = ChatHelper::FormatRace(notification.attackerRace);
        std::string cls = ChatHelper::FormatClass(notification.attackerClass);
        // The speaker describes the level the way their screen shows it: a
        // number, or the skull every player calls "??".
        std::string level = LevelPhrase(notification.attackerLevelText);

        TriggerContext trigger;
        trigger.kind = TRIGGER_GAME_EVENT;
        trigger.eventType = escalation ? "defense_escalation" : "defense_callout";
        trigger.chatType = CHAT_MSG_CHANNEL;
        // Short names; the say tool resolves them to the joined channel
        // ("LocalDefense" matches "LocalDefense - Redridge Mountains").
        trigger.channelName = escalation ? "WorldDefense" : "LocalDefense";
        trigger.defenseChannel = true;
        if (escalation)
            trigger.message = Acore::StringFormat("the enemy {}, a {} {} {}, has killed {} of your side in {}"
                " and nobody has stopped them yet. Raise the alarm in the faction-wide WorldDefense channel so help"
                " comes",
                notification.attackerName, level, race, cls, notification.killCount,
                notification.areaName);
        else
        {
            // Tell the model what was actually seen - "attacking <area>" for
            // someone genuinely present hostile, "prowling" for a known
            // ganker merely sighted - so the alarm matches the events.
            std::string enemyDesc = Acore::StringFormat("{}, a {} {} {}",
                notification.attackerName, level, race, cls);
            std::string spotted;
            switch (notification.activity)
            {
                case WpvpCalloutActivity::AttackingPlayer:
                {
                    // These callouts only fire for a victim genuinely
                    // outmatched - by level or by numbers - so tell the model
                    // which it was: both levels, and the headcount when the
                    // victim is outnumbered.
                    uint8 attackerCount = notification.victimAttackerCount;
                    if (notification.victimName == bot->GetName())
                        spotted = attackerCount > 1
                            ? Acore::StringFormat("you are being attacked near {} by {} enemies; one of them is {}",
                                notification.areaName, attackerCount, enemyDesc)
                            : Acore::StringFormat("you are being attacked near {} by an enemy: {}",
                                notification.areaName, enemyDesc);
                    else
                        spotted = attackerCount > 1
                            ? Acore::StringFormat(
                                "you spotted {} enemies attacking {}, a level {} ally, near {}; one of them is {}",
                                attackerCount, notification.victimName, notification.victimLevel,
                                notification.areaName, enemyDesc)
                            : Acore::StringFormat("you spotted an enemy attacking {}, a level {} ally, near {}: {}",
                                notification.victimName, notification.victimLevel, notification.areaName, enemyDesc);
                    break;
                }
                case WpvpCalloutActivity::Prowling:
                    spotted = Acore::StringFormat("you spotted an enemy the defense channels already warned about"
                        " prowling near {}: {}", notification.areaName, enemyDesc);
                    break;
                default:
                    spotted = Acore::StringFormat("you spotted an enemy attacking {}: {}", notification.areaName,
                        enemyDesc);
                    break;
            }
            trigger.message = Acore::StringFormat("{}. Raise the alarm in the zone's LocalDefense channel - name the"
                " attacker and where they are", spotted);
        }
        Dispatch::Submit(bot, nullptr, std::move(trigger));
    }
}

void AddSC_llm_event()
{
    new ModLlm::LlmEventScript();
    new ModLlm::LlmHealedScript();
    new ModLlm::LlmGroupScript();
    RegisterWpvpCalloutListener(&ModLlm::OnWpvpCallout);
}
