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
        // Each outcome is also recorded (parallel to `calls`) for the
        // feedback round below.
        std::vector<Outcome> outcomes;
        outcomes.reserve(calls.size());
        auto finish = [&](ToolCall const& call, bool ok, std::string outcome, std::string result = "")
        {
            if (sLlmConfig->debugEnabled)
                LOG_INFO("module.llm", "Bot {} tool '{}' {}: {}", bot->GetName(), call.name, call.arguments, outcome);
            else
                LOG_DEBUG("module.llm", "Bot {} tool '{}' {}: {}", bot->GetName(), call.name, call.arguments, outcome);
            outcomes.push_back({ ok, std::move(outcome), std::move(result) });
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

            context.result.clear();
            if (spec->execute(context, args, error))
            {
                anySucceeded = true;
                std::string result = std::move(context.result);
                finish(call, true, result.empty() ? "executed" : "returned data", std::move(result));
            }
            else
            {
                finish(call, false, Acore::StringFormat("failed: {}", error));
            }
        }

        SubmitToolFeedback(bot, actor, outcomes);

        return anySucceeded || calls.empty();
    }

    // Follow-up requests carry failed calls' errors and read tools' data back
    // as tool-result messages, so the model can pick an alternative action or
    // talk about what it just looked up. Rounds are capped so a model that
    // keeps reading or failing cannot loop. Only genuine tool calls qualify -
    // the rescued bare-content say has no real tool_call id to reference.
    void LlmToolOperation::SubmitToolFeedback(Player* bot, Player* actor,
        std::vector<Outcome> const& outcomes) const
    {
        constexpr uint32 MAX_FOLLOW_UP_ROUNDS = 2;
        if (_round >= MAX_FOLLOW_UP_ROUNDS || _toolCalls.empty())
            return;

        bool anyFailed = false;
        bool anyResult = false;
        for (Outcome const& outcome : outcomes)
        {
            anyFailed = anyFailed || !outcome.ok;
            anyResult = anyResult || !outcome.result.empty();
        }
        if (!anyResult && !(anyFailed && sLlmConfig->errorFeedbackEnabled))
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
        {
            Outcome const& outcome = outcomes[i];
            std::string content;
            if (!outcome.ok)
                content = "error: " + outcome.text
                    + ". Nobody in the world saw this attempt; pick a different action, or do nothing.";
            else if (!outcome.result.empty())
                content = outcome.result;
            else
                content = "ok";
            extra.push_back({
                { "role", "tool" },
                { "tool_call_id", _toolCalls[i].id },
                { "content", std::move(content) }
            });
        }

        LlmRequest followUp;
        followUp.snapshot = ContextBuilder::Build(bot, actor, _trigger);
        followUp.tools = sLlmToolRegistry->BuildToolsArray(_trigger.kind, bot, actor, &_trigger);
        followUp.trigger = _trigger;
        followUp.extraMessages = std::move(extra);
        followUp.round = _round + 1;

        if (sLlmClient->Submit(std::move(followUp)))
        {
            if (sLlmConfig->debugEnabled)
                LOG_INFO("module.llm", "Bot {} tool results fed back to the model (round {})",
                    bot->GetName(), _round + 1);
        }
    }
}
