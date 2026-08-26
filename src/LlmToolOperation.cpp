/*
 * This file is part of mod-llm for Felworld. Released under the MIT license
 * (see the LICENSE file at the module root).
 */

#include "LlmToolOperation.h"

#include "Common.h"
#include "ContextBuilder.h"
#include "FelworldEvents.h"
#include "Guild.h"
#include "GuildMgr.h"
#include "LlmClient.h"
#include "LlmConfig.h"
#include "LlmTools.h"
#include "Log.h"
#include "Metric.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotMgr.h"
#include "StringFormat.h"
#include "ToolRegistry.h"
#include "Util.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <numeric>
#include <unordered_map>
#include <utility>

namespace ModLlm
{
    namespace
    {
        // A mustered wave answers one call for help with at most
        // Defense.MaxSpeakers "omw" messages: later responders still go,
        // they just post nothing. Keyed by room (channel + team) with a
        // window generous enough to cover one wave of picked bots replying
        // at their own LLM pace. Operations all run on the world thread, so
        // plain statics suffice.
        bool ClaimDefenseSpeakSlot(TriggerContext const& trigger)
        {
            constexpr uint32 windowMs = 60 * IN_MILLISECONDS;
            static std::unordered_map<std::string, std::pair<uint32, uint32>> windows; // start, count

            std::string const& key = trigger.roomKey.empty() ? trigger.channelName : trigger.roomKey;
            uint32 now = getMSTime();
            auto& [startMs, count] = windows[key];
            if (!startMs || now - startMs > windowMs)
            {
                startMs = now;
                count = 0;
            }
            if (count >= sLlmConfig->defenseMaxSpeakers)
                return false;
            ++count;
            return true;
        }

        // Ad prompts end in "if nothing is worth posting, do nothing", and
        // small models often narrate that choice as prose ("none worth
        // pushing rn") instead of staying silent. Bare content only earns
        // the say rescue on an ad trigger when it matches the format the
        // prompt demanded - a WTS/WTB line for a trade ad, a line naming the
        // guild for a guild ad; the rest is deliberation, dropped as the
        // do-nothing it means. An explicit say call is untouched: a model
        // that called the tool committed to speaking.
        bool LooksLikeRequestedAd(Player* bot, TriggerContext const& trigger,
            std::string const& content)
        {
            if (trigger.tradeAd)
            {
                if (content.find("{item:") != std::string::npos)
                    return true;
                size_t start = content.find_first_not_of(" \t\r\n\"'*");
                if (start == std::string::npos)
                    return false;
                std::string_view lead = std::string_view(content).substr(start);
                return StringStartsWithI(lead, "wts") || StringStartsWithI(lead, "wtb");
            }

            if (trigger.guildAd)
            {
                Guild* guild = sGuildMgr->GetGuildById(bot->GetGuildId());
                return guild && StringContainsStringI(content, guild->GetName());
            }

            return true;
        }
    }

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
        // optionally rescue that as a say. Ad triggers gate the rescue on
        // the content actually looking like the requested ad. A round that
        // follows one which already spoke gets no rescue: withholding the say
        // tool would otherwise just push the repeat into prose
        // (felworld/mod-llm#59).
        if (calls.empty() && !_bareContent.empty() && sLlmConfig->treatBareContentAsSay && !_spoke)
        {
            // Prose is the one place the model can address the wrong tool: it
            // types the note it meant to save alongside (or instead of) the
            // chat line, and the rescue would speak the scratchpad to the
            // party. Route each half where it was addressed.
            std::string spoken = _bareContent;
            std::string note = LlmTools::ExtractInlineNote(spoken);

            if (!spoken.empty())
            {
                if (!LooksLikeRequestedAd(bot, _trigger, spoken))
                    LOG_INFO("module.llm", "Bot {} bare content dropped, not the ad the prompt asked"
                        " for: '{}'", bot->GetName(), spoken);
                else
                {
                    nlohmann::json args;
                    args["message"] = spoken;
                    // Synthetic call: the fabricated id lets a failure feed
                    // back like any genuine call's would.
                    calls.push_back({ "say", args.dump(), "call_say" });
                }
            }

            if (!note.empty())
            {
                nlohmann::json args;
                // Prose carries no slug; the note's own opening stands in, so
                // a repeat note about the same subject lands on the same key.
                args["slug"] = note.substr(0, note.find_first_of(".,;", 1));
                args["content"] = note;
                calls.push_back({ "remember", args.dump(), "call_remember" });
            }
        }

        // Tool outcomes surface at INFO under LLM.Debug.Enable; the default
        // logger config swallows DEBUG, which makes silent bots undebuggable.
        // Each outcome is also recorded (parallel to `calls`) for the
        // feedback round below.
        std::vector<Outcome> outcomes(calls.size());
        auto finish = [&](size_t index, bool ok, std::string outcome, std::string result = "")
        {
            ToolCall const& call = calls[index];
            if (sLlmConfig->debugEnabled)
                LOG_INFO("module.llm", "Bot {} tool '{}' {}: {}", bot->GetName(), call.name, call.arguments, outcome);
            else
                LOG_DEBUG("module.llm", "Bot {} tool '{}' {}: {}", bot->GetName(), call.name, call.arguments, outcome);

            METRIC_VALUE("llm_tool_calls", 1, METRIC_TAG("tool", call.name), METRIC_TAG("outcome", ok ? "ok" : "error"));
            Felworld::LogEvent(bot->GetGUID(), "llm_tool",
                nlohmann::json({ { "tool", call.name }, { "args", call.arguments },
                                 { "outcome", outcome }, { "ok", ok } }).dump());

            outcomes[index] = { ok, std::move(outcome), std::move(result) };
        };

        // Defense-channel replies are enforced in code, not just prompted:
        // the one message that belongs is the "omw" beside a successful
        // go_defend, so go_defend runs first and channel-bound says are
        // swallowed without it. The reply guidance asks for the same thing,
        // but a model that ignores it and types a decline anyway must not
        // reach the channel. Says pointed elsewhere (whisper, guild, ...)
        // pass - they cannot land in the alarm channel.
        bool defenseReply = _trigger.kind == TRIGGER_CHAT_CHANNEL && _trigger.defenseChannel;
        std::vector<size_t> order(calls.size());
        std::iota(order.begin(), order.end(), 0);
        if (defenseReply)
            std::stable_partition(order.begin(), order.end(),
                [&](size_t index) { return calls[index].name == "go_defend"; });

        auto channelBoundSay = [&](ToolCall const& call)
        {
            if (call.name != "say")
                return false;
            nlohmann::json args = nlohmann::json::parse(call.arguments, nullptr, false);
            if (args.is_discarded())
                return false; // malformed arguments get their normal error below
            std::string destination = args.value("destination", "");
            return destination.empty() || destination == "channel";
        };

        bool goDefendSucceeded = false;
        bool anySucceeded = false;
        std::vector<std::string> spokenArgs; // speech calls already sent this round
        for (size_t index : order)
        {
            ToolCall const& call = calls[index];

            // An earlier round of this exchange already spoke, so this one was
            // handed a toolbox without the speech tools - but a model is free
            // to call a tool it was never offered, and that call is exactly
            // the repeated line the withholding was for (felworld/mod-llm#59).
            // Swallowed rather than failed: the words the bot owed the world
            // are already out there.
            if (_spoke && ToolRegistry::IsSpeechTool(call.name))
            {
                finish(index, true, "swallowed: already replied out loud earlier in this exchange");
                continue;
            }

            // A model that lists the same speech call twice in one response
            // meant to say it once. Only a byte-identical repeat is dropped -
            // two different messages (a party line and a whisper, say) are a
            // real player's prerogative.
            if (ToolRegistry::IsSpeechTool(call.name)
                && std::find(spokenArgs.begin(), spokenArgs.end(), call.arguments) != spokenArgs.end())
            {
                finish(index, true, "swallowed: this exact message already went out");
                continue;
            }

            // Swallowed, not failed: an error would invite the model to try
            // the message again in the feedback round, and silence is exactly
            // the outcome the channel wants.
            if (defenseReply && channelBoundSay(call))
            {
                if (!goDefendSucceeded)
                {
                    finish(index, true, "swallowed: defense-channel reply without go_defend");
                    continue;
                }
                if (!ClaimDefenseSpeakSlot(_trigger))
                {
                    finish(index, true, "swallowed: the channel heard enough on-my-ways already");
                    continue;
                }
            }

            ToolSpec const* spec = sLlmToolRegistry->Find(call.name);
            if (!spec)
            {
                finish(index, false, "unknown tool");
                continue;
            }

            if (!(spec->triggerMask & _trigger.kind))
            {
                finish(index, false, "not allowed for this trigger");
                continue;
            }

            if (spec->requiresActor && !actor)
            {
                finish(index, false, "the actor is gone");
                continue;
            }

            nlohmann::json args = nlohmann::json::parse(call.arguments, nullptr, false);
            if (args.is_discarded())
            {
                finish(index, false, "malformed arguments");
                continue;
            }

            std::string error;
            if (!ToolRegistry::ValidateArgs(spec->parameters, args, error))
            {
                finish(index, false, Acore::StringFormat("rejected: {}", error));
                continue;
            }

            context.result.clear();
            if (spec->execute(context, args, error))
            {
                anySucceeded = true;
                if (call.name == "go_defend")
                    goDefendSucceeded = true;
                std::string result = std::move(context.result);
                finish(index, true, result.empty() ? "executed" : "returned data", std::move(result));
                // Recorded only here, past every swallow and failure above:
                // the follow-up round withholds the speech tools on it, and it
                // must mean the bot was genuinely heard.
                if (ToolRegistry::IsSpeechTool(call.name))
                {
                    outcomes[index].spoke = true;
                    spokenArgs.push_back(call.arguments);
                }
            }
            else
            {
                finish(index, false, Acore::StringFormat("failed: {}", error));
            }
        }

        SubmitToolFeedback(bot, actor, calls, outcomes);

        return anySucceeded || calls.empty();
    }

    // Follow-up requests carry failed calls' errors and read tools' data back
    // as tool-result messages, so the model can pick an alternative action or
    // talk about what it just looked up. Rounds are capped so a model that
    // keeps reading or failing cannot loop. The rescued bare-content say
    // qualifies too, under its fabricated call id: a prose-answering model
    // otherwise never hears why its words went nowhere - the cross-faction
    // "use the emote tool" redirect in particular was thrown away for
    // exactly the models that needed it (felworld/mod-llm#36).
    void LlmToolOperation::SubmitToolFeedback(Player* bot, Player* actor,
        std::vector<ToolCall> const& calls, std::vector<Outcome> const& outcomes) const
    {
        constexpr uint32 MAX_FOLLOW_UP_ROUNDS = 2;
        if (_round >= MAX_FOLLOW_UP_ROUNDS || calls.empty())
            return;

        bool anyFailed = false;
        bool anyResult = false;
        bool spoke = _spoke;
        for (Outcome const& outcome : outcomes)
        {
            anyFailed = anyFailed || !outcome.ok;
            anyResult = anyResult || !outcome.result.empty();
            spoke = spoke || outcome.spoke;
        }
        if (!anyResult && !(anyFailed && sLlmConfig->errorFeedbackEnabled))
            return;

        nlohmann::json toolCallsJson = nlohmann::json::array();
        for (ToolCall const& call : calls)
            toolCallsJson.push_back({
                { "id", call.id },
                { "type", "function" },
                { "function", { { "name", call.name }, { "arguments", call.arguments } } }
            });

        // On the rescue path the prose already sits in the synthetic say's
        // arguments - repeating it as content would read as a second copy.
        nlohmann::json extra = nlohmann::json::array();
        extra.push_back({
            { "role", "assistant" },
            { "content", _toolCalls.empty() ? "" : _bareContent },
            { "tool_calls", std::move(toolCallsJson) }
        });
        // Failed attempts are invisible to everyone in the world; saying so
        // keeps the model from working the failure into its chat.
        for (size_t i = 0; i < calls.size(); ++i)
        {
            Outcome const& outcome = outcomes[i];
            std::string content;
            if (!outcome.ok)
                content = "error: " + outcome.text
                    + ". Nobody in the world saw this attempt; pick a different action, or do nothing.";
            else if (!outcome.result.empty())
                content = outcome.result;
            else if (outcome.spoke)
                content = "ok: your message was sent and everyone heard it. You have already replied out"
                    " loud this turn; use the remaining tools only if something else still needs doing.";
            else
                content = "ok";
            extra.push_back({
                { "role", "tool" },
                { "tool_call_id", calls[i].id },
                { "content", std::move(content) }
            });
        }

        // A round that already spoke goes back to the model without the speech
        // tools. Left in, they are the likeliest thing a small model reaches
        // for a second time, and it answers the same prompt from the same
        // context - so the party hears the same line twice
        // (felworld/mod-llm#59). The rest of the toolbox stays, so the model
        // can still act on what a read tool just told it.
        LlmRequest followUp;
        followUp.snapshot = ContextBuilder::Build(bot, actor, _trigger);
        followUp.tools = sLlmToolRegistry->BuildToolsArray(_trigger.kind, bot, actor, &_trigger, !spoke);
        followUp.trigger = _trigger;
        followUp.extraMessages = std::move(extra);
        followUp.round = _round + 1;
        followUp.spoke = spoke;

        if (sLlmClient->Submit(std::move(followUp)))
        {
            if (sLlmConfig->debugEnabled)
                LOG_INFO("module.llm", "Bot {} tool results fed back to the model (round {})",
                    bot->GetName(), _round + 1);
        }
    }
}
