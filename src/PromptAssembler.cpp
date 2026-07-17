/*
 * This file is part of mod-llm for Felworld. Released under the MIT license
 * (see the LICENSE file at the module root).
 */

#include "PromptAssembler.h"

#include "LlmConfig.h"
#include "Log.h"
#include "SentimentStore.h"
#include "StringFormat.h"

#include <fmt/args.h>
#include <fmt/format.h>

namespace ModLlm::PromptAssembler
{
    namespace
    {
        std::string SafeFormat(std::string const& templ, fmt::dynamic_format_arg_store<fmt::format_context> const& args,
            std::string const& fallback)
        {
            try
            {
                return fmt::vformat(templ, args);
            }
            catch (std::exception const& e)
            {
                LOG_ERROR("module.llm", "Bad prompt template '{}': {}", templ, e.what());
                return fallback;
            }
        }

        std::string BuildSystemMessage(ContextSnapshot const& snapshot)
        {
            fmt::dynamic_format_arg_store<fmt::format_context> args;
            args.push_back(fmt::arg("bot_name", snapshot.botName));
            args.push_back(fmt::arg("bot_level", snapshot.botLevel));
            args.push_back(fmt::arg("bot_class", snapshot.botClass));
            args.push_back(fmt::arg("bot_race", snapshot.botRace));
            args.push_back(fmt::arg("bot_faction", snapshot.botFaction));
            args.push_back(fmt::arg("bot_area", snapshot.botArea));
            args.push_back(fmt::arg("bot_zone", snapshot.botZone));
            args.push_back(fmt::arg("bot_group", snapshot.botGroup));
            args.push_back(fmt::arg("bot_guild", snapshot.botGuild));
            args.push_back(fmt::arg("style_examples", sLlmConfig->promptStyleExamples));

            std::string fallback = Acore::StringFormat(
                "You are {}, a level {} {} {} in World of Warcraft. React with the available tools or do nothing.",
                snapshot.botName, snapshot.botLevel, snapshot.botRace, snapshot.botClass);

            return SafeFormat(sLlmConfig->promptSystem, args, fallback);
        }

        std::string BuildSentimentLine(ContextSnapshot const& snapshot)
        {
            if (!snapshot.hasSentiment)
                return "";

            fmt::dynamic_format_arg_store<fmt::format_context> args;
            args.push_back(fmt::arg("actor_name", snapshot.actorName));
            args.push_back(fmt::arg("sentiment_word", SentimentStore::Describe(snapshot.sentimentValue)));
            args.push_back(fmt::arg("sentiment_value", Acore::StringFormat("{:.2f}", snapshot.sentimentValue)));

            return SafeFormat(sLlmConfig->promptSentimentLine, args, "");
        }

        std::string BuildHistoryBlock(ContextSnapshot const& snapshot)
        {
            std::string block;
            if (!snapshot.overheardHistory.empty())
                block += "Recently overheard around you:\n" + snapshot.overheardHistory;
            if (!snapshot.roomHistory.empty())
                block += "Recent messages here:\n" + snapshot.roomHistory;
            if (!snapshot.pairHistory.empty())
                block += "Your conversation with " + snapshot.actorName + ":\n" + snapshot.pairHistory;
            return block;
        }

        std::string BuildUserMessage(ContextSnapshot const& snapshot, TriggerContext const& trigger)
        {
            fmt::dynamic_format_arg_store<fmt::format_context> args;
            args.push_back(fmt::arg("sentiment_line", BuildSentimentLine(snapshot)));
            args.push_back(fmt::arg("history_block", BuildHistoryBlock(snapshot)));
            args.push_back(fmt::arg("channel_label", snapshot.channelLabel));
            args.push_back(fmt::arg("actor_name", snapshot.actorName));
            args.push_back(fmt::arg("actor_level", snapshot.actorLevel));
            args.push_back(fmt::arg("actor_class", snapshot.actorClass));
            args.push_back(fmt::arg("actor_race", snapshot.actorRace));
            args.push_back(fmt::arg("message", trigger.message));
            args.push_back(fmt::arg("reply_guidance", snapshot.replyGuidance));
            args.push_back(fmt::arg("environment", snapshot.environment));

            std::string const* templ = nullptr;
            switch (trigger.kind)
            {
                case TRIGGER_EMOTE:
                    templ = &sLlmConfig->promptEmote;
                    break;
                case TRIGGER_GAME_EVENT:
                    templ = &sLlmConfig->promptEvent;
                    break;
                case TRIGGER_INITIATIVE:
                    templ = &sLlmConfig->promptInitiative;
                    break;
                default:
                    templ = &sLlmConfig->promptChat;
                    break;
            }

            std::string fallback = trigger.message.empty()
                ? "Decide whether to act."
                : Acore::StringFormat("{}: \"{}\"", snapshot.actorName, trigger.message);

            return SafeFormat(*templ, args, fallback);
        }
    }

    nlohmann::json BuildMessages(ContextSnapshot const& snapshot, TriggerContext const& trigger)
    {
        return nlohmann::json::array({
            { { "role", "system" }, { "content", BuildSystemMessage(snapshot) } },
            { { "role", "user" }, { "content", BuildUserMessage(snapshot, trigger) } }
        });
    }
}
