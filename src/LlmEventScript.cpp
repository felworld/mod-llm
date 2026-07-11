/*
 * This file is part of mod-llm for Felworld. Released under the MIT license
 * (see the LICENSE file at the module root).
 */

#include "BotSelector.h"
#include "Creature.h"
#include "Item.h"
#include "ItemTemplate.h"
#include "LlmConfig.h"
#include "LlmDispatch.h"
#include "Player.h"
#include "QuestDef.h"
#include "Random.h"
#include "ScriptMgr.h"
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
                if (!BotSelector::IsRealPlayer(actor)
                    && !BotSelector::HasRealPlayerNearby(bot, sLlmConfig->eventRealPlayerDistance))
                    continue;
                if (IsOnCooldown(bot->GetGUID()))
                    continue;

                TriggerContext trigger;
                trigger.kind = TRIGGER_GAME_EVENT;
                trigger.eventType = eventType;
                trigger.message = description;
                if (!Dispatch::Submit(bot, actor != bot ? actor : nullptr, std::move(trigger)))
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
}

void AddSC_llm_event()
{
    new ModLlm::LlmEventScript();
}
