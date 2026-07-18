/*
 * This file is part of mod-llm for Felworld. Released under the MIT license
 * (see the LICENSE file at the module root).
 */

#include "BotSelector.h"
#include "Creature.h"
#include "Group.h"
#include "Item.h"
#include "ItemTemplate.h"
#include "LlmConfig.h"
#include "LlmDispatch.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "QuestDef.h"
#include "Random.h"
#include "ScriptMgr.h"
#include "SharedDefines.h"
#include "StringFormat.h"

#include <chrono>
#include <mutex>
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
            PLAYERHOOK_ON_STORE_NEW_ITEM
        }) { }

        void OnPlayerCreatureKill(Player* killer, Creature* killed) override
        {
            DispatchEvent(killer, "creature_kill", sLlmConfig->eventChanceKill,
                Acore::StringFormat("{} killed {}", killer->GetName(), killed->GetName()));
        }

        void OnPlayerPVPKill(Player* killer, Player* killed) override
        {
            DispatchEvent(killer, "pvp_kill", sLlmConfig->eventChancePvpKill,
                Acore::StringFormat("{} killed {} in PvP", killer->GetName(), killed->GetName()));
        }

        void OnPlayerJustDied(Player* player) override
        {
            DispatchEvent(player, "death", sLlmConfig->eventChanceDeath,
                Acore::StringFormat("{} just died", player->GetName()));
        }

        void OnPlayerCompleteQuest(Player* player, Quest const* quest) override
        {
            DispatchEvent(player, "quest_complete", sLlmConfig->eventChanceQuestComplete,
                Acore::StringFormat("{} completed the quest \"{}\"", player->GetName(), quest->GetTitle()));
        }

        void OnPlayerLevelChanged(Player* player, uint8 oldLevel) override
        {
            if (player->GetLevel() <= oldLevel)
                return;
            DispatchEvent(player, "level_up", sLlmConfig->eventChanceLevelUp,
                Acore::StringFormat("{} reached level {}", player->GetName(), player->GetLevel()));
        }

        void OnPlayerDuelRequest(Player* target, Player* challenger) override
        {
            DispatchEvent(target, "duel_request", sLlmConfig->eventChanceDuel,
                Acore::StringFormat("{} challenged {} to a duel", challenger->GetName(), target->GetName()),
                challenger);
        }

        void OnPlayerDuelEnd(Player* winner, Player* loser, DuelCompleteType type) override
        {
            if (type != DUEL_WON)
                return;
            DispatchEvent(winner, "duel_end", sLlmConfig->eventChanceDuel,
                Acore::StringFormat("{} won a duel against {}", winner->GetName(), loser->GetName()));
        }

        void OnPlayerAchievementComplete(Player* player, AchievementEntry const* achievement) override
        {
            char const* name = achievement->name[0];
            DispatchEvent(player, "achievement", sLlmConfig->eventChanceAchievement,
                Acore::StringFormat("{} earned the achievement \"{}\"", player->GetName(), name ? name : "?"));
        }

        void OnPlayerStoreNewItem(Player* player, Item* item, uint32 /*count*/) override
        {
            ItemTemplate const* proto = item->GetTemplate();
            if (!proto || proto->Quality < sLlmConfig->eventLootMinQuality)
                return;
            DispatchEvent(player, "loot", sLlmConfig->eventChanceLoot,
                Acore::StringFormat("{} obtained [{}]", player->GetName(), proto->Name1));
        }

    private:
        void DispatchEvent(Player* source, char const* eventType, uint32 chance, std::string description,
            Player* actorOverride = nullptr)
        {
            if (!sLlmConfig->IsEnabled() || !sLlmConfig->eventEnabled || !chance)
                return;

            Player* actor = actorOverride ? actorOverride : source;

            // Include the source itself: a bot may react to its own level-up.
            std::vector<Player*> bots = BotSelector::SelectNearby(source, sLlmConfig->eventBotDistance,
                16, true);

            uint32 dispatched = 0;
            for (Player* bot : bots)
            {
                if (dispatched >= sLlmConfig->eventMaxBotsPerEvent)
                    break;
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
                else if (urand(0, 99) < sLlmConfig->eventChannelChance)
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
}

void AddSC_llm_event()
{
    new ModLlm::LlmEventScript();
    new ModLlm::LlmHealedScript();
    new ModLlm::LlmGroupScript();
}
