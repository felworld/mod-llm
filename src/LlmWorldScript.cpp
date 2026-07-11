/*
 * This file is part of mod-llm for Felworld. Released under the MIT license
 * (see the LICENSE file at the module root).
 */

#include "BotSelector.h"
#include "GameTime.h"
#include "HistoryStore.h"
#include "LlmClient.h"
#include "LlmConfig.h"
#include "LlmDispatch.h"
#include "LlmTools.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "Random.h"
#include "ScriptMgr.h"
#include "SentimentStore.h"

#include <unordered_map>

namespace ModLlm
{
    // Owns the module lifecycle (config, stores, worker pool) and the
    // initiative scheduler that lets bots act unprompted. Everything here runs
    // on the world thread.
    class LlmWorldScript : public WorldScript
    {
    public:
        LlmWorldScript() : WorldScript("LlmWorldScript", {
            WORLDHOOK_ON_AFTER_CONFIG_LOAD,
            WORLDHOOK_ON_STARTUP,
            WORLDHOOK_ON_UPDATE,
            WORLDHOOK_ON_SHUTDOWN
        }) { }

        void OnAfterConfigLoad(bool /*reload*/) override
        {
            sLlmConfig->Load();
        }

        void OnStartup() override
        {
            // Always bring the machinery up, even when LLM.Enable=0: the
            // worker threads idle and `.llm enable` can flip the switch at
            // runtime without a restart.
            LlmTools::RegisterDefaultTools();
            sLlmSentimentStore->Load();
            sLlmHistoryStore->Load();
            sLlmClient->Start();
        }

        void OnUpdate(uint32 diff) override
        {
            if (!sLlmConfig->IsEnabled())
                return;

            _initiativeTimer += diff;
            if (_initiativeTimer >= 5000)
            {
                _initiativeTimer = 0;
                UpdateInitiative();
            }

            _sentimentSaveTimer += diff;
            if (_sentimentSaveTimer >= sLlmConfig->sentimentSaveIntervalSeconds * IN_MILLISECONDS)
            {
                _sentimentSaveTimer = 0;
                sLlmSentimentStore->SaveDirty();
            }

            _historySaveTimer += diff;
            if (_historySaveTimer >= sLlmConfig->historySaveIntervalSeconds * IN_MILLISECONDS)
            {
                _historySaveTimer = 0;
                sLlmHistoryStore->SaveDirty();
            }
        }

        void OnShutdown() override
        {
            sLlmClient->Stop();
            sLlmSentimentStore->SaveDirty();
            sLlmHistoryStore->SaveDirty();
        }

    private:
        void UpdateInitiative()
        {
            if (!sLlmConfig->initiativeEnabled)
                return;

            time_t now = GameTime::GetGameTime().count();
            uint32 submitted = 0;

            for (auto const& [guid, player] : ObjectAccessor::GetPlayers())
            {
                if (submitted >= sLlmConfig->initiativeMaxBotsPerTick)
                    break;
                if (!player->IsInWorld() || BotSelector::IsRealPlayer(player))
                    continue;

                auto [it, inserted] = _nextInitiative.try_emplace(guid.GetRawValue(), 0);
                if (inserted || !it->second)
                {
                    it->second = now + NextDelay();
                    continue;
                }
                if (it->second > now)
                    continue;

                it->second = now + NextDelay();

                if (urand(0, 99) >= sLlmConfig->initiativeChance)
                    continue;
                if (player->IsInCombat())
                    continue;
                if (!BotSelector::HasRealPlayerNearby(player, sLlmConfig->initiativeRealPlayerDistance))
                    continue;

                TriggerContext trigger;
                trigger.kind = TRIGGER_INITIATIVE;
                if (Dispatch::Submit(player, nullptr, std::move(trigger)))
                    ++submitted;
            }
        }

        time_t NextDelay() const
        {
            return time_t(urand(sLlmConfig->initiativeMinIntervalSeconds,
                std::max(sLlmConfig->initiativeMinIntervalSeconds, sLlmConfig->initiativeMaxIntervalSeconds)));
        }

        uint32 _initiativeTimer = 0;
        uint32 _sentimentSaveTimer = 0;
        uint32 _historySaveTimer = 0;
        std::unordered_map<uint64, time_t> _nextInitiative;
    };
}

void AddSC_llm_world()
{
    new ModLlm::LlmWorldScript();
}
