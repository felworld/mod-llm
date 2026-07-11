/*
 * This file is part of mod-llm for Felworld. Released under the MIT license
 * (see the LICENSE file at the module root).
 */

#include "LlmDispatch.h"

#include "ContextBuilder.h"
#include "LlmClient.h"
#include "Player.h"

namespace ModLlm::Dispatch
{
    bool Submit(Player* bot, Player* actor, TriggerContext trigger)
    {
        trigger.botGuid = bot->GetGUID();
        if (actor)
        {
            trigger.actorGuid = actor->GetGUID();
            trigger.actorName = actor->GetName();
        }

        LlmRequest request;
        request.snapshot = ContextBuilder::Build(bot, actor, trigger);
        request.toolMask = trigger.kind;
        request.trigger = std::move(trigger);

        return sLlmClient->Submit(std::move(request));
    }
}
