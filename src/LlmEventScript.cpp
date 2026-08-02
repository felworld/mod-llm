/*
 * This file is part of mod-llm for Felworld. Released under the MIT license
 * (see the LICENSE file at the module root).
 */

#include "BotSelector.h"
#include "ChatHelper.h"
#include "Creature.h"
#include "Group.h"
#include "HistoryStore.h"
#include "Item.h"
#include "ItemTemplate.h"
#include "LlmConfig.h"
#include "LlmDispatch.h"
#include "Map.h"
#include "ObjectAccessor.h"
#include "Player.h"
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
            DispatchEvent(killer, "creature_kill", sLlmConfig->eventChanceKill,
                ActorAware(killer->GetGUID(),
                    Acore::StringFormat("you killed {}", killed->GetName()),
                    Acore::StringFormat("{} killed {}", killer->GetName(), killed->GetName())),
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

        void OnPlayerCompleteQuest(Player* player, Quest const* quest) override
        {
            DispatchEvent(player, "quest_complete", sLlmConfig->eventChanceQuestComplete,
                ActorAware(player->GetGUID(),
                    Acore::StringFormat("you completed the quest \"{}\"", quest->GetTitle()),
                    Acore::StringFormat("{} completed the quest \"{}\"", player->GetName(), quest->GetTitle())));
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

        void OnPlayerAchievementComplete(Player* player, AchievementEntry const* achievement) override
        {
            char const* name = achievement->name[0];
            DispatchEvent(player, "achievement", sLlmConfig->eventChanceAchievement,
                ActorAware(player->GetGUID(),
                    Acore::StringFormat("you earned the achievement \"{}\"", name ? name : "?"),
                    Acore::StringFormat("{} earned the achievement \"{}\"", player->GetName(), name ? name : "?")));
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
                if (!bot || BotSelector::IsRealPlayer(bot))
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

        // Only feats that carry their own story - a ding, an achievement, a
        // rare drop - are worth retelling to the zone-wide General channel.
        // Play-by-play (mob pulls, deaths, duels, PvP kills) is invisible to
        // readers across the zone: the prompt asks the model to retell or
        // stay silent, but small models still produce "nice pulls", so the
        // gate is enforced here and those comments stay in local /say.
        static bool IsZoneChannelWorthy(char const* eventType)
        {
            std::string_view type(eventType);
            return type == "level_up" || type == "achievement" || type == "loot";
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
            if (BotSelector::IsRealPlayer(bot))
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
                // to the zone's General channel - resolving it needs the world
                // thread, so the wish rides along and the delayed dispatch
                // binds it (falling back to /say when the bot has no channel
                // or no human reads it). The rest is said aloud, which is
                // only worth doing with a human in earshot.
                Group* group = bot->GetGroup();
                if (group && actor->GetGroup() == group && !group->isBGGroup() && !group->isBFGroup())
                {
                    if (!BotSelector::GroupHasRealPlayer(group))
                        continue;
                    trigger.chatType = group->isRaidGroup() ? CHAT_MSG_RAID : CHAT_MSG_PARTY;
                    trigger.roomKey = Acore::StringFormat("group:{}", group->GetGUID().GetCounter());
                }
                else if (IsZoneChannelWorthy(eventType) && urand(0, 99) < sLlmConfig->eventChannelChance)
                    trigger.wantZoneChannel = true;
                else if (!BotSelector::HasRealPlayerNearby(bot, sLlmConfig->sayDistance))
                    continue;

                if (trigger.wantZoneChannel)
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
            if (!healer || !bot || BotSelector::IsRealPlayer(bot))
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
            if (!bot || BotSelector::IsRealPlayer(bot))
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
        if (!bot || BotSelector::IsRealPlayer(bot))
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
                if (player && BotSelector::IsRealPlayer(player) && player->GetZoneId() == notification.zoneId)
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

        TriggerContext trigger;
        trigger.kind = TRIGGER_GAME_EVENT;
        trigger.eventType = escalation ? "defense_escalation" : "defense_callout";
        trigger.chatType = CHAT_MSG_CHANNEL;
        // Short names; the say tool resolves them to the joined channel
        // ("LocalDefense" matches "LocalDefense - Redridge Mountains").
        trigger.channelName = escalation ? "WorldDefense" : "LocalDefense";
        trigger.defenseChannel = true;
        if (escalation)
            trigger.message = Acore::StringFormat("the enemy {}, a level {} {} {}, has killed {} of your side in {}"
                " and nobody has stopped them yet. Raise the alarm in the faction-wide WorldDefense channel so help"
                " comes",
                notification.attackerName, notification.attackerLevel, race, cls, notification.killCount,
                notification.areaName);
        else
        {
            // Tell the model what was actually seen - "attacking <area>" for
            // someone genuinely present hostile, "prowling" for a known
            // ganker merely sighted - so the alarm matches the events.
            std::string enemyDesc = Acore::StringFormat("{}, a level {} {} {}",
                notification.attackerName, notification.attackerLevel, race, cls);
            std::string spotted;
            switch (notification.activity)
            {
                case WpvpCalloutActivity::AttackingPlayer:
                    spotted = notification.victimName == bot->GetName()
                        ? Acore::StringFormat("you are being attacked near {} by an enemy: {}", notification.areaName,
                            enemyDesc)
                        : Acore::StringFormat("you spotted an enemy attacking {} near {}: {}",
                            notification.victimName, notification.areaName, enemyDesc);
                    break;
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
