/*
 * This file is part of mod-llm for Felworld. Released under the MIT license
 * (see the LICENSE file at the module root).
 */

#include "LlmToolOperation.h"

#include "LlmConfig.h"
#include "LlmTools.h"
#include "Log.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotMgr.h"
#include "ToolRegistry.h"

#include <nlohmann/json.hpp>

namespace ModLlm
{
    bool LlmToolOperation::IsValid() const
    {
        return sLlmConfig->IsEnabled();
    }

    bool LlmToolOperation::Execute()
    {
        Player* bot = ObjectAccessor::FindPlayer(_trigger.botGuid);
        if (!bot || !bot->IsInWorld())
            return false;

        PlayerbotAI* botAI = sPlayerbotsMgr->GetPlayerbotAI(bot);
        if (!botAI || !botAI->IsBotAI())
            return false;

        Player* actor = _trigger.actorGuid ? ObjectAccessor::FindPlayer(_trigger.actorGuid) : nullptr;

        ToolExecContext context;
        context.bot = bot;
        context.ai = botAI;
        context.actor = actor;
        context.trigger = &_trigger;

        std::vector<ToolCall> calls = _toolCalls;

        // Weaker models sometimes answer in prose instead of calling a tool;
        // optionally rescue that as a say.
        if (calls.empty() && !_bareContent.empty() && sLlmConfig->treatBareContentAsSay)
        {
            nlohmann::json args;
            args["message"] = _bareContent;
            calls.push_back({ "say", args.dump() });
        }

        bool anySucceeded = false;
        for (ToolCall const& call : calls)
        {
            ToolSpec const* spec = sLlmToolRegistry->Find(call.name);
            if (!spec)
            {
                LOG_DEBUG("module.llm", "Bot {} called unknown tool '{}'", bot->GetName(), call.name);
                continue;
            }

            if (!(spec->triggerMask & _trigger.kind))
            {
                LOG_DEBUG("module.llm", "Bot {} called tool '{}' outside its trigger mask", bot->GetName(), call.name);
                continue;
            }

            if (spec->requiresActor && !actor)
            {
                LOG_DEBUG("module.llm", "Bot {} called tool '{}' but the actor is gone", bot->GetName(), call.name);
                continue;
            }

            nlohmann::json args = nlohmann::json::parse(call.arguments, nullptr, false);
            if (args.is_discarded())
            {
                LOG_DEBUG("module.llm", "Bot {} tool '{}' has malformed arguments", bot->GetName(), call.name);
                continue;
            }

            std::string error;
            if (!ToolRegistry::ValidateArgs(spec->parameters, args, error))
            {
                LOG_DEBUG("module.llm", "Bot {} tool '{}' rejected: {}", bot->GetName(), call.name, error);
                continue;
            }

            if (spec->execute(context, args, error))
            {
                anySucceeded = true;
            }
            else
            {
                LOG_DEBUG("module.llm", "Bot {} tool '{}' failed: {}", bot->GetName(), call.name, error);
            }
        }

        return anySucceeded || calls.empty();
    }
}
