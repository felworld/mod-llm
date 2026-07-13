/*
 * This file is part of mod-llm for Felworld. Released under the MIT license
 * (see the LICENSE file at the module root).
 */

#include "LlmDispatch.h"

#include "ContextBuilder.h"
#include "LlmClient.h"
#include "LlmConfig.h"
#include "Log.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "ToolRegistry.h"

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
        trigger.botGuid = bot->GetGUID();
        if (actor)
        {
            trigger.actorGuid = actor->GetGUID();
            trigger.actorName = actor->GetName();
        }

        _pending.push_back({ delayMs, std::move(trigger) });
    }

    void UpdateDelayed(uint32 diff)
    {
        for (size_t i = 0; i < _pending.size();)
        {
            if (_pending[i].remainingMs > diff)
            {
                _pending[i].remainingMs -= diff;
                ++i;
                continue;
            }

            TriggerContext trigger = std::move(_pending[i].trigger);
            _pending.erase(_pending.begin() + i);

            Player* bot = ObjectAccessor::FindPlayer(trigger.botGuid);
            if (!bot || !bot->IsInWorld())
                continue;
            if (sLlmConfig->skipInCombat && bot->IsInCombat())
                continue;

            Player* actor = trigger.actorGuid ? ObjectAccessor::FindPlayer(trigger.actorGuid) : nullptr;
            Submit(bot, actor, std::move(trigger));
        }
    }

    void ClearDelayed()
    {
        _pending.clear();
    }
}
