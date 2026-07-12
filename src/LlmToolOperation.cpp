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
#include "StringFormat.h"
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

        PlayerbotAI* botAI = sPlayerbotsMgr.GetPlayerbotAI(bot);
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

        // Tool outcomes surface at INFO under LLM.Debug.Enable; the default
        // logger config swallows DEBUG, which makes silent bots undebuggable.
        auto logOutcome = [&](std::string const& toolName, std::string const& outcome)
        {
            if (sLlmConfig->debugEnabled)
                LOG_INFO("module.llm", "Bot {} tool '{}': {}", bot->GetName(), toolName, outcome);
            else
                LOG_DEBUG("module.llm", "Bot {} tool '{}': {}", bot->GetName(), toolName, outcome);
        };

        bool anySucceeded = false;
        for (ToolCall const& call : calls)
        {
            ToolSpec const* spec = sLlmToolRegistry->Find(call.name);
            if (!spec)
            {
                logOutcome(call.name, "unknown tool");
                continue;
            }

            if (!(spec->triggerMask & _trigger.kind))
            {
                logOutcome(call.name, "not allowed for this trigger");
                continue;
            }

            if (spec->requiresActor && !actor)
            {
                logOutcome(call.name, "the actor is gone");
                continue;
            }

            nlohmann::json args = nlohmann::json::parse(call.arguments, nullptr, false);
            if (args.is_discarded())
            {
                logOutcome(call.name, "malformed arguments");
                continue;
            }

            std::string error;
            if (!ToolRegistry::ValidateArgs(spec->parameters, args, error))
            {
                logOutcome(call.name, Acore::StringFormat("rejected: {}", error));
                continue;
            }

            if (spec->execute(context, args, error))
            {
                anySucceeded = true;
                logOutcome(call.name, "executed");
            }
            else
            {
                logOutcome(call.name, Acore::StringFormat("failed: {}", error));
            }
        }

        return anySucceeded || calls.empty();
    }
}
