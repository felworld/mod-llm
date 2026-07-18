/*
 * This file is part of mod-llm for Felworld. Released under the MIT license
 * (see the LICENSE file at the module root).
 */

#include "LlmDispatch.h"

#include "BotSelector.h"
#include "ContextBuilder.h"
#include "LlmClient.h"
#include "LlmConfig.h"
#include "Log.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "ToolRegistry.h"

#include <mutex>
#include <vector>

namespace ModLlm::Dispatch
{
    namespace
    {
        struct PendingDispatch
        {
            uint32 remainingMs = 0;
            TriggerContext trigger;
        };

        // Guarded: SubmitDelayed can be called from map-update threads (e.g.
        // the group-join hook fires from a bot's AI update) while UpdateDelayed
        // sweeps on the world thread.
        std::mutex _pendingMutex;
        std::vector<PendingDispatch> _pending;
    }

    bool Submit(Player* bot, Player* actor, TriggerContext trigger)
    {
        trigger.botGuid = bot->GetGUID();
        if (actor)
        {
            trigger.actorGuid = actor->GetGUID();
            trigger.actorName = actor->GetName();
        }

        // What triggered a bot surfaces at INFO under LLM.Debug.Enable, so the
        // debug log pairs each "Bot X response" with what the bot reacted to.
        if (!trigger.message.empty())
        {
            std::string source = trigger.actorName.empty() ? "" : " from " + trigger.actorName;
            if (sLlmConfig->debugEnabled)
                LOG_INFO("module.llm", "Bot {} trigger {}{}: '{}'",
                    bot->GetName(), TriggerKindName(trigger.kind), source, trigger.message);
            else
                LOG_DEBUG("module.llm", "Bot {} trigger {}{}: '{}'",
                    bot->GetName(), TriggerKindName(trigger.kind), source, trigger.message);
        }

        LlmRequest request;
        request.snapshot = ContextBuilder::Build(bot, actor, trigger);
        request.tools = sLlmToolRegistry->BuildToolsArray(trigger.kind, bot, actor);
        request.trigger = std::move(trigger);

        return sLlmClient->Submit(std::move(request));
    }

    void SubmitDelayed(Player* bot, Player* actor, TriggerContext trigger, uint32 delayMs)
    {
        if (actor)
        {
            trigger.actorGuid = actor->GetGUID();
            trigger.actorName = actor->GetName();
        }

        SubmitDelayed(bot->GetGUID(), std::move(trigger), delayMs);
    }

    void SubmitDelayed(ObjectGuid botGuid, TriggerContext trigger, uint32 delayMs)
    {
        trigger.botGuid = botGuid;

        std::lock_guard<std::mutex> lock(_pendingMutex);
        _pending.push_back({ delayMs, std::move(trigger) });
    }

    void UpdateDelayed(uint32 diff)
    {
        // Collect due triggers under the lock, submit outside it: Submit
        // touches game state and the HTTP client and must not hold the lock.
        std::vector<TriggerContext> due;
        {
            std::lock_guard<std::mutex> lock(_pendingMutex);
            for (size_t i = 0; i < _pending.size();)
            {
                if (_pending[i].remainingMs > diff)
                {
                    _pending[i].remainingMs -= diff;
                    ++i;
                    continue;
                }

                due.push_back(std::move(_pending[i].trigger));
                _pending.erase(_pending.begin() + i);
            }
        }

        for (TriggerContext& trigger : due)
        {
            Player* bot = ObjectAccessor::FindPlayer(trigger.botGuid);
            if (!bot || !bot->IsInWorld())
                continue;
            if (sLlmConfig->skipInCombat && bot->IsInCombat())
                continue;

            // Deferred from a map-thread hook: now that we are on the world
            // thread the zone channel can be resolved. An unbound trigger
            // falls back to /say - worth doing only with a human in earshot.
            if (trigger.wantZoneChannel)
            {
                trigger.wantZoneChannel = false;
                if (!BotSelector::BindZoneChannel(bot, trigger)
                    && !BotSelector::HasRealPlayerNearby(bot, sLlmConfig->sayDistance))
                    continue;
            }

            Player* actor = trigger.actorGuid ? ObjectAccessor::FindPlayer(trigger.actorGuid) : nullptr;
            Submit(bot, actor, std::move(trigger));
        }
    }

    void ClearDelayed()
    {
        std::lock_guard<std::mutex> lock(_pendingMutex);
        _pending.clear();
    }
}
