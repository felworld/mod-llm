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
#include "QuestDef.h"
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

        // True if the bot's quest log holds any of the quests linked in the
        // routed message - the strongest routing signal there is: "anyone for
        // [quest]?" is meant for exactly these characters.
        bool OnLinkedQuest(Player* bot, std::vector<uint32> const& questIds)
        {
            for (uint32 questId : questIds)
                if (bot->FindQuestSlot(questId) < MAX_QUEST_LOG_SIZE)
                    return true;
            return false;
        }

        std::string RosterLine(Player* bot, bool onLinkedQuest)
        {
            std::string classDesc = ChatHelper::FormatClass(bot->getClass());
            std::string spec = AiFactory::GetPlayerSpecName(bot);
            if (!spec.empty())
                classDesc = spec + " " + classDesc;

            return Acore::StringFormat("- {} (level {} {}, {}{})",
                bot->GetName(), bot->GetLevel(), classDesc, RoleWord(bot),
                onLinkedQuest ? ", on that quest too" : "");
        }

        // One routing request: the assembled prompt plus everything its
        // worker-thread callback needs to turn the reply into dispatches.
        struct RouteRequest
        {
            std::string prompt;
            std::string label;          // log name: "Group router" / "Say router" / "Room router"
            std::vector<Candidate> roster;
            TriggerContext trigger;
            size_t maxPick;             // cap on picks: MaxBotsToPick, or the
                                        // (lower) BotTrigger cap for bot senders
            bool recordPairLines;       // picked bots remember the actor's line (say/yell)
        };

        // Bot-to-bot exchanges branch by this cap on every hop, so a bot's
        // message picks fewer responders than a real player's: player-seeded
        // flurries are the point, bot-seeded trees are noise.
        size_t MaxPickFor(Player* sender)
        {
            return BotSelector::IsRealPlayer(sender)
                ? sLlmConfig->maxBotsToPick
                : std::min<size_t>(sLlmConfig->maxBotsToPick, sLlmConfig->botTriggerMaxBotsToPick);
        }

        // The routing prompt cannot grow with the crowd: cap the roster,
        // keeping a bot addressed by name (the deterministic always-pick)
        // and anyone on a quest the message links ahead of the cut. Call
        // after shuffling, so the drop is a fair sample of the room.
        void PromoteMentionAndCap(std::vector<Player*>& candidates, TriggerContext const& trigger)
        {
            if (!trigger.linkedQuests.empty())
                std::stable_partition(candidates.begin(), candidates.end(),
                    [&trigger](Player* bot) { return OnLinkedQuest(bot, trigger.linkedQuests); });

            for (size_t i = 0; i < candidates.size(); ++i)
            {
                if (BotSelector::MentionsName(trigger.message, candidates[i]->GetName()))
                {
                    std::rotate(candidates.begin(), candidates.begin() + i, candidates.begin() + i + 1);
                    break;
                }
            }

            size_t cap = std::max<uint32>(sLlmConfig->routerMaxRoster, 1);
            if (candidates.size() > cap)
                candidates.resize(cap);
        }

        bool SubmitRoute(RouteRequest&& route)
        {
            size_t maxPick = route.maxPick;
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
                    // Unparseable reply: route to nobody (name-mentions above
                    // excepted). No quiet degradation to dice - a router
                    // model producing garbage should be loud in the logs, not
                    // masked by fallback chatter.
                    LOG_ERROR("module.llm", "{} reply not parseable, routing to nobody: '{}'",
                        route.label, response.content);
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

        // Shuffled up front so the roster cap drops a fair sample and the
        // model sees no meaningful ordering.
        std::vector<Player*> shuffled = candidates;
        Acore::Containers::RandomShuffle(shuffled);
        PromoteMentionAndCap(shuffled, trigger);

        bool bg = trigger.chatType == CHAT_MSG_BATTLEGROUND || trigger.chatType == CHAT_MSG_BATTLEGROUND_LEADER;

        RouteRequest route;
        route.maxPick = MaxPickFor(sender);
        std::string rosterText;
        for (Player* bot : shuffled)
        {
            route.roster.push_back({ bot->GetGUID(), bot->GetName() });
            if (!rosterText.empty())
                rosterText += "\n";
            rosterText += RosterLine(bot, OnLinkedQuest(bot, trigger.linkedQuests));
        }

        fmt::dynamic_format_arg_store<fmt::format_context> args;
        args.push_back(fmt::arg("channel_label", bg ? "battleground" : "raid"));
        args.push_back(fmt::arg("actor_name", trigger.actorName));
        args.push_back(fmt::arg("message", trigger.message));
        args.push_back(fmt::arg("roster", rosterText));
        args.push_back(fmt::arg("max_picks", route.maxPick));

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
        route.recordPairLines = false;
        return SubmitRoute(std::move(route));
    }

    bool RouteSayMessage(Player* sender, std::vector<Player*> const& candidates, TriggerContext trigger)
    {
        trigger.actorGuid = sender->GetGUID();
        trigger.actorName = sender->GetName();

        // Shuffled up front so the roster cap drops a fair sample and the
        // model sees no meaningful ordering.
        std::vector<Player*> shuffled = candidates;
        Acore::Containers::RandomShuffle(shuffled);
        PromoteMentionAndCap(shuffled, trigger);

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
        route.maxPick = MaxPickFor(sender);
        std::string rosterText;
        for (Player* bot : shuffled)
        {
            route.roster.push_back({ bot->GetGUID(), bot->GetName() });
            if (!rosterText.empty())
                rosterText += "\n";
            rosterText += RosterLine(bot, OnLinkedQuest(bot, trigger.linkedQuests));
        }

        fmt::dynamic_format_arg_store<fmt::format_context> args;
        args.push_back(fmt::arg("actor_name", trigger.actorName));
        args.push_back(fmt::arg("message", trigger.message));
        args.push_back(fmt::arg("roster", rosterText));
        args.push_back(fmt::arg("history_block", historyBlock));
        args.push_back(fmt::arg("max_picks", route.maxPick));

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
        // Direct exchanges (say/yell) also feed the pair transcript.
        route.recordPairLines = true;
        return SubmitRoute(std::move(route));
    }

    bool RouteRoomMessage(Player* sender, std::vector<Player*> const& candidates, TriggerContext trigger)
    {
        trigger.actorGuid = sender->GetGUID();
        trigger.actorName = sender->GetName();

        // Label and note derive from the trigger, so every call site
        // describes the same room the same way. Defense channels carry an
        // extra note: per-candidate dice could never hold a faction-wide
        // alarm channel quiet, but a router told that nearly every alarm is
        // read in silence can.
        std::string roomLabel;
        std::string roomNote;
        if (trigger.kind == TRIGGER_CHAT_GUILD)
            roomLabel = "guild chat";
        else if (trigger.defenseChannel)
        {
            roomLabel = Acore::StringFormat("the \"{}\" defense channel", trigger.channelName);
            roomNote = "It is an alarm channel where enemy attacks are reported: pick a character only "
                "if they are named or would genuinely drop what they are doing and go help.\n";
        }
        else
            roomLabel = Acore::StringFormat("the \"{}\" channel", trigger.channelName);

        // Shuffled up front so the roster cap drops a fair sample and the
        // model sees no meaningful ordering.
        std::vector<Player*> shuffled = candidates;
        Acore::Containers::RandomShuffle(shuffled);
        PromoteMentionAndCap(shuffled, trigger);

        // The room transcript ends with the message being routed (it was
        // recorded before us).
        std::string history = sLlmHistoryStore->FormatRoom(trigger.roomKey,
            sLlmConfig->historyMaxRoomLines);

        std::string historyBlock;
        if (!history.empty())
            historyBlock = "Recently said there:\n" + history;

        RouteRequest route;
        route.maxPick = MaxPickFor(sender);
        std::string rosterText;
        for (Player* bot : shuffled)
        {
            route.roster.push_back({ bot->GetGUID(), bot->GetName() });
            if (!rosterText.empty())
                rosterText += "\n";
            rosterText += RosterLine(bot, OnLinkedQuest(bot, trigger.linkedQuests));
        }

        // A linked quest softens the silence bias: "anyone for [quest]?" is
        // aimed squarely at whoever has it in their log.
        if (std::any_of(shuffled.begin(), shuffled.end(),
            [&trigger](Player* bot) { return OnLinkedQuest(bot, trigger.linkedQuests); }))
            roomNote += "The message asks about a quest: a character marked \"on that quest too\""
                " would naturally answer.\n";

        fmt::dynamic_format_arg_store<fmt::format_context> args;
        args.push_back(fmt::arg("actor_name", trigger.actorName));
        args.push_back(fmt::arg("message", trigger.message));
        args.push_back(fmt::arg("roster", rosterText));
        args.push_back(fmt::arg("history_block", historyBlock));
        args.push_back(fmt::arg("room_label", roomLabel));
        args.push_back(fmt::arg("room_note", roomNote));
        args.push_back(fmt::arg("max_picks", route.maxPick));

        try
        {
            route.prompt = fmt::vformat(sLlmConfig->promptRoomRouter, args);
        }
        catch (std::exception const& e)
        {
            LOG_ERROR("module.llm", "Bad room-router prompt template '{}': {}",
                sLlmConfig->promptRoomRouter, e.what());
            return false;
        }

        route.label = "Room router";
        route.trigger = std::move(trigger);
        // Room lines were already recorded via AddRoomLine.
        route.recordPairLines = false;
        return SubmitRoute(std::move(route));
    }
}
