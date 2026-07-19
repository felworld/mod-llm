/*
 * This file is part of mod-llm for Felworld. Released under the MIT license
 * (see the LICENSE file at the module root).
 */

#include "LlmToolOperation.h"

#include "ContextBuilder.h"
#include "LlmClient.h"
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

#include <utility>

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
        // Each outcome is also recorded (parallel to `calls`) for the error
        // feedback round below.
        std::vector<std::pair<bool, std::string>> outcomes;
        outcomes.reserve(calls.size());
        auto finish = [&](ToolCall const& call, bool ok, std::string outcome)
        {
            if (sLlmConfig->debugEnabled)
                LOG_INFO("module.llm", "Bot {} tool '{}' {}: {}", bot->GetName(), call.name, call.arguments, outcome);
            else
                LOG_DEBUG("module.llm", "Bot {} tool '{}' {}: {}", bot->GetName(), call.name, call.arguments, outcome);
            outcomes.emplace_back(ok, std::move(outcome));
        };

        bool anySucceeded = false;
        for (ToolCall const& call : calls)
        {
            ToolSpec const* spec = sLlmToolRegistry->Find(call.name);
            if (!spec)
            {
                finish(call, false, "unknown tool");
                continue;
            }

            if (!(spec->triggerMask & _trigger.kind))
            {
                finish(call, false, "not allowed for this trigger");
                continue;
            }

            if (spec->requiresActor && !actor)
            {
                finish(call, false, "the actor is gone");
                continue;
            }

            nlohmann::json args = nlohmann::json::parse(call.arguments, nullptr, false);
            if (args.is_discarded())
            {
                finish(call, false, "malformed arguments");
                continue;
            }

            std::string error;
            if (!ToolRegistry::ValidateArgs(spec->parameters, args, error))
            {
                finish(call, false, Acore::StringFormat("rejected: {}", error));
                continue;
            }

            if (spec->execute(context, args, error))
            {
                anySucceeded = true;
                finish(call, true, "executed");
            }
            else
            {
                finish(call, false, Acore::StringFormat("failed: {}", error));
            }
        }

        SubmitErrorFeedback(bot, actor, outcomes);

        return anySucceeded || calls.empty();
    }

    // One follow-up request per trigger (round-capped) carrying the failed
    // calls' errors as tool-result messages, so the model can pick an
    // alternative action. Only genuine tool calls qualify - the rescued
    // bare-content say has no real tool_call id to reference.
    void LlmToolOperation::SubmitErrorFeedback(Player* bot, Player* actor,
        std::vector<std::pair<bool, std::string>> const& outcomes) const
    {
        if (!sLlmConfig->errorFeedbackEnabled || _round > 0 || _toolCalls.empty())
            return;

        bool anyFailed = false;
        for (auto const& [ok, outcome] : outcomes)
            anyFailed = anyFailed || !ok;
        if (!anyFailed)
            return;

        nlohmann::json toolCallsJson = nlohmann::json::array();
        for (ToolCall const& call : _toolCalls)
            toolCallsJson.push_back({
                { "id", call.id },
                { "type", "function" },
                { "function", { { "name", call.name }, { "arguments", call.arguments } } }
            });

        nlohmann::json extra = nlohmann::json::array();
        extra.push_back({
            { "role", "assistant" },
            { "content", _bareContent },
            { "tool_calls", std::move(toolCallsJson) }
        });
        // Failed attempts are invisible to everyone in the world; saying so
        // keeps the model from working the failure into its chat.
        for (size_t i = 0; i < _toolCalls.size(); ++i)
            extra.push_back({
                { "role", "tool" },
                { "tool_call_id", _toolCalls[i].id },
                { "content", outcomes[i].first ? "ok"
                    : "error: " + outcomes[i].second
                        + ". Nobody in the world saw this attempt; pick a different action, or do nothing." }
            });

        LlmRequest followUp;
        followUp.snapshot = ContextBuilder::Build(bot, actor, _trigger);
        followUp.tools = sLlmToolRegistry->BuildToolsArray(_trigger.kind, bot, actor, &_trigger);
        followUp.trigger = _trigger;
        followUp.extraMessages = std::move(extra);
        followUp.round = _round + 1;

        if (sLlmClient->Submit(std::move(followUp)))
        {
            if (sLlmConfig->debugEnabled)
                LOG_INFO("module.llm", "Bot {} tool errors fed back to the model", bot->GetName());
        }
    }
}
