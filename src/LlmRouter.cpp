/*
 * This file is part of mod-llm for Felworld. Released under the MIT license
 * (see the LICENSE file at the module root).
 */

#include "LlmRouter.h"

#include "AiFactory.h"
#include "BotSelector.h"
#include "ChatHelper.h"
#include "Common.h"
#include "Containers.h"
#include "HistoryStore.h"
#include "LlmClient.h"
#include "LlmConfig.h"
#include "LlmDispatch.h"
#include "Log.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "Random.h"
#include "SharedDefines.h"
#include "StringFormat.h"

#include <fmt/args.h>
#include <fmt/format.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>

namespace ModLlm::Router
{
    namespace
    {
        // What the worker-thread callback is allowed to know about a
        // candidate: no Player pointers cross into the async pipeline.
        struct Candidate
        {
            ObjectGuid guid;
            std::string name;
        };

        bool EqualsIgnoreCase(std::string const& a, std::string const& b)
        {
            return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(),
                [](unsigned char x, unsigned char y) { return std::tolower(x) == std::tolower(y); });
        }

        std::string RoleWord(Player* bot)
        {
            if (PlayerbotAI::IsTank(bot, true))
                return "tank";
            if (PlayerbotAI::IsHeal(bot, true))
                return "healer";
            return "damage dealer";
        }

        std::string RosterLine(Player* bot)
        {
            std::string classDesc = ChatHelper::FormatClass(bot->getClass());
            std::string spec = AiFactory::GetPlayerSpecName(bot);
            if (!spec.empty())
                classDesc = spec + " " + classDesc;

            return Acore::StringFormat("- {} (level {} {}, {})",
                bot->GetName(), bot->GetLevel(), classDesc, RoleWord(bot));
        }

        // One routing request: the assembled prompt plus everything its
        // worker-thread callback needs to turn the reply into dispatches.
        struct RouteRequest
        {
            std::string prompt;
            std::string label;          // log name: "Group router" / "Say router"
            std::vector<Candidate> roster;
            TriggerContext trigger;
            uint32 fallbackChance;      // per-candidate percent when the reply is unparseable
            bool recordPairLines;       // picked bots remember the actor's line (say/yell)
        };

        bool SubmitRoute(RouteRequest&& route)
        {
            size_t maxPick = sLlmConfig->maxBotsToPick;
            uint32 staggerMs = sLlmConfig->chatStaggerSeconds * IN_MILLISECONDS;

            LlmRequest request;
            request.snapshot.botName = Acore::StringFormat("router:{}",
                route.trigger.roomKey.empty() ? "say:" + route.trigger.actorName : route.trigger.roomKey);
            request.trigger = route.trigger;
            request.customMessages = nlohmann::json::array({
                { { "role", "user" }, { "content", route.prompt } }
            });

            // Runs on an HTTP worker thread: strings and GUIDs only, and the
            // picked triggers re-enter the game through the (thread-safe)
            // delayed-dispatch queue, which resolves players on the world
            // thread. HistoryStore appends are mutex-guarded.
            request.onResponse = [route = std::move(route), maxPick, staggerMs]
                (LlmResponse const& response)
            {
                TriggerContext const& trigger = route.trigger;

                std::vector<size_t> picks;
                auto addPick = [&](size_t index)
                {
                    if (picks.size() < maxPick && std::find(picks.begin(), picks.end(), index) == picks.end())
                        picks.push_back(index);
                };

                // The deterministic guarantee routing must not lose: a bot
                // addressed by name is always asked (first mention wins).
                for (size_t i = 0; i < route.roster.size(); ++i)
                {
                    if (BotSelector::MentionsName(trigger.message, route.roster[i].name))
                    {
                        addPick(i);
                        break;
                    }
                }

                if (std::optional<std::vector<std::string>> names = ParseRouterReply(response.content))
                {
                    for (std::string const& name : *names)
                        for (size_t i = 0; i < route.roster.size(); ++i)
                            if (EqualsIgnoreCase(name, route.roster[i].name))
                                addPick(i);
                }
                else
                {
                    // Unparseable reply: degrade to the pre-router behaviour,
                    // rolling the caller's dice down the (shuffled) roster.
                    LOG_WARN("module.llm", "{} reply not parseable, rolling dice: '{}'",
                        route.label, response.content);
                    for (size_t i = 0; i < route.roster.size() && picks.size() < maxPick; ++i)
                        if (urand(0, 99) < route.fallbackChance)
                            addPick(i);
                }

                if (sLlmConfig->debugEnabled)
                {
                    std::string picked;
                    for (size_t i : picks)
                    {
                        if (!picked.empty())
                            picked += ", ";
                        picked += route.roster[i].name;
                    }
                    LOG_INFO("module.llm", "{} for '{}' picked [{}]", route.label, trigger.message, picked);
                }

                uint32 index = 0;
                for (size_t i : picks)
                {
                    if (route.recordPairLines && trigger.actorGuid)
                        sLlmHistoryStore->AddPairLine(route.roster[i].guid, trigger.actorGuid, false,
                            trigger.actorName, trigger.message);
                    Dispatch::SubmitDelayed(route.roster[i].guid, trigger, index * staggerMs);
                    ++index;
                }
            };

            return sLlmClient->Submit(std::move(request));
        }
    }

    std::optional<std::vector<std::string>> ParseRouterReply(std::string const& content)
    {
        // The array should be the whole reply, but small models sometimes
        // wrap it in prose. Character names cannot contain brackets, so try
        // each [...] pair until one parses as a JSON array.
        for (size_t start = content.find('['); start != std::string::npos;
            start = content.find('[', start + 1))
        {
            size_t end = content.find(']', start);
            if (end == std::string::npos)
                break;

            nlohmann::json parsed = nlohmann::json::parse(
                content.substr(start, end - start + 1), nullptr, false);
            if (!parsed.is_array())
                continue;

            std::vector<std::string> names;
            for (nlohmann::json const& entry : parsed)
                if (entry.is_string())
                    names.push_back(entry.get<std::string>());
            return names;
        }

        return std::nullopt;
    }

    bool RouteGroupMessage(Player* sender, std::vector<Player*> const& candidates, TriggerContext trigger)
    {
        trigger.actorGuid = sender->GetGUID();
        trigger.actorName = sender->GetName();

        // Shuffled up front so the no-parse fallback (first MaxBotsToPick
        // entries) degrades to the old random pick.
        std::vector<Player*> shuffled = candidates;
        Acore::Containers::RandomShuffle(shuffled);

        bool bg = trigger.chatType == CHAT_MSG_BATTLEGROUND || trigger.chatType == CHAT_MSG_BATTLEGROUND_LEADER;

        RouteRequest route;
        std::string rosterText;
        for (Player* bot : shuffled)
        {
            route.roster.push_back({ bot->GetGUID(), bot->GetName() });
            if (!rosterText.empty())
                rosterText += "\n";
            rosterText += RosterLine(bot);
        }

        fmt::dynamic_format_arg_store<fmt::format_context> args;
        args.push_back(fmt::arg("channel_label", bg ? "battleground" : "raid"));
        args.push_back(fmt::arg("actor_name", trigger.actorName));
        args.push_back(fmt::arg("message", trigger.message));
        args.push_back(fmt::arg("roster", rosterText));
        args.push_back(fmt::arg("max_picks", sLlmConfig->maxBotsToPick));

        try
        {
            route.prompt = fmt::vformat(sLlmConfig->promptRouter, args);
        }
        catch (std::exception const& e)
        {
            LOG_ERROR("module.llm", "Bad router prompt template '{}': {}", sLlmConfig->promptRouter, e.what());
            return false;
        }

        route.label = "Group router";
        route.trigger = std::move(trigger);
        // Pre-router behaviour was asking the first shuffled bots up to the
        // cap, i.e. a 100% roll per entry.
        route.fallbackChance = 100;
        route.recordPairLines = false;
        return SubmitRoute(std::move(route));
    }

    bool RouteSayMessage(Player* sender, std::vector<Player*> const& candidates, TriggerContext trigger)
    {
        trigger.actorGuid = sender->GetGUID();
        trigger.actorName = sender->GetName();

        // Shuffled up front so the no-parse fallback (dice down the roster)
        // matches the router-off behaviour.
        std::vector<Player*> shuffled = candidates;
        Acore::Containers::RandomShuffle(shuffled);

        // Everyone in earshot of the sender heard roughly the same recent
        // conversation, so the overheard buffer of the candidate nearest the
        // sender stands in for "what was recently said here". Its last line
        // is the message being routed (RecordSpeech ran before us).
        Player* nearest = *std::min_element(shuffled.begin(), shuffled.end(),
            [sender](Player* a, Player* b) { return sender->GetDistance(a) < sender->GetDistance(b); });
        std::string history = sLlmHistoryStore->FormatOverheard(nearest->GetGUID(),
            sLlmConfig->historyMaxOverheardLines);

        std::string historyBlock;
        if (!history.empty())
            historyBlock = "Recently said nearby:\n" + history;

        RouteRequest route;
        std::string rosterText;
        for (Player* bot : shuffled)
        {
            route.roster.push_back({ bot->GetGUID(), bot->GetName() });
            if (!rosterText.empty())
                rosterText += "\n";
            rosterText += RosterLine(bot);
        }

        fmt::dynamic_format_arg_store<fmt::format_context> args;
        args.push_back(fmt::arg("actor_name", trigger.actorName));
        args.push_back(fmt::arg("message", trigger.message));
        args.push_back(fmt::arg("roster", rosterText));
        args.push_back(fmt::arg("history_block", historyBlock));
        args.push_back(fmt::arg("max_picks", sLlmConfig->maxBotsToPick));

        try
        {
            route.prompt = fmt::vformat(sLlmConfig->promptSayRouter, args);
        }
        catch (std::exception const& e)
        {
            LOG_ERROR("module.llm", "Bad say-router prompt template '{}': {}",
                sLlmConfig->promptSayRouter, e.what());
            return false;
        }

        route.label = "Say router";
        route.trigger = std::move(trigger);
        // Router-off behaviour rolls the say dice per candidate.
        route.fallbackChance = BotSelector::ReplyChance(TRIGGER_CHAT_SAY, false);
        // Direct exchanges (say/yell) also feed the pair transcript.
        route.recordPairLines = true;
        return SubmitRoute(std::move(route));
    }
}
