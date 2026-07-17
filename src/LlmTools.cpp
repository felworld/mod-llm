/*
 * This file is part of mod-llm for Felworld. Released under the MIT license
 * (see the LICENSE file at the module root).
 */

#include "LlmTools.h"

#include "Channel.h"
#include "ChannelMgr.h"
#include "Chat.h"
#include "Group.h"
#include "HistoryStore.h"
#include "LlmConfig.h"
#include "LlmTrigger.h"
#include "Log.h"
#include "MemoryStore.h"
#include "Overhear.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "SharedDefines.h"
#include "TextEmoteCatalog.h"
#include "ToolRegistry.h"
#include "WorldPacket.h"
#include "WorldSession.h"

#include <cctype>

namespace ModLlm::LlmTools
{
    namespace
    {
        // Playerbots has no battleground-chat helper, so mirror its
        // SayToParty/SayToRaid: build the packet and hand it to every real
        // player in the group (other bots hear replies through history).
        bool SayToBattleground(ToolExecContext& context, std::string const& message)
        {
            if (!context.bot->GetGroup())
                return false;

            WorldPacket data;
            ChatHandler::BuildChatPacket(data, CHAT_MSG_BATTLEGROUND, message, LANG_UNIVERSAL,
                CHAT_TAG_NONE, context.bot->GetGUID(), context.bot->GetName());

            for (Player* receiver : context.ai->GetRealPlayersInGroup())
                receiver->GetSession()->SendPacket(&data);

            return true;
        }

        // Sends `message` into the exact channel named by the trigger.
        // PlayerbotAI::SayToChannel would re-resolve builtin channels by the
        // bot's current zone and report success even for a channel the bot
        // is not on, where Channel::Say quietly drops the line.
        bool SendToChannel(Player* bot, std::string const& channelName, std::string const& message)
        {
            if (ChannelMgr* mgr = ChannelMgr::forTeam(bot->GetTeamId()))
            {
                Channel* channel = mgr->GetChannel(channelName, bot, false);
                if (channel && bot->IsInChannel(channel))
                {
                    channel->Say(bot->GetGUID(), message, LANG_UNIVERSAL);
                    return true;
                }
            }
            return false;
        }

        // Sends `message` back into the channel the trigger came from. The
        // audience is bound by the trigger, never chosen by the model.
        bool RouteSay(ToolExecContext& context, std::string const& message, std::string& error)
        {
            TriggerContext const& trigger = *context.trigger;
            bool sent = false;

            // How the message went out, for the overhear fan-out below.
            bool spokeAloud = false; // /say or /yell, audible around the bot
            bool yelled = false;
            bool spokeInChannel = false;

            switch (trigger.kind)
            {
                case TRIGGER_CHAT_WHISPER:
                    sent = context.ai->Whisper(message, trigger.actorName);
                    break;
                case TRIGGER_CHAT_PARTY:
                    if (trigger.chatType == CHAT_MSG_BATTLEGROUND || trigger.chatType == CHAT_MSG_BATTLEGROUND_LEADER)
                        sent = SayToBattleground(context, message);
                    else if (trigger.chatType == CHAT_MSG_RAID || trigger.chatType == CHAT_MSG_RAID_LEADER
                        || trigger.chatType == CHAT_MSG_RAID_WARNING)
                        sent = context.ai->SayToRaid(message);
                    else
                        sent = context.ai->SayToParty(message);
                    break;
                case TRIGGER_CHAT_GUILD:
                    sent = context.ai->SayToGuild(message);
                    break;
                case TRIGGER_CHAT_CHANNEL:
                    sent = spokeInChannel = SendToChannel(context.bot, trigger.channelName, message);
                    break;
                default:
                    // Game events normally reply in /say, but a trigger can
                    // bind another audience (a group greeting on join, an
                    // initiative remark pointed at the zone channel).
                    if (trigger.chatType == CHAT_MSG_CHANNEL)
                        sent = spokeInChannel = SendToChannel(context.bot, trigger.channelName, message);
                    else if (trigger.chatType == CHAT_MSG_RAID)
                        sent = context.ai->SayToRaid(message);
                    else if (trigger.chatType == CHAT_MSG_PARTY)
                        sent = context.ai->SayToParty(message);
                    else if (trigger.chatType == CHAT_MSG_YELL)
                        sent = spokeAloud = yelled = context.ai->Yell(message);
                    else
                        sent = spokeAloud = context.ai->Say(message);
                    break;
            }

            if (!sent)
            {
                error = "message could not be delivered";
                return false;
            }

            if (trigger.actorGuid)
                sLlmHistoryStore->AddPairLine(trigger.botGuid, trigger.actorGuid, true,
                    context.bot->GetName(), message);
            if (!trigger.roomKey.empty())
                sLlmHistoryStore->AddRoomLine(trigger.roomKey, trigger.botGuid,
                    context.bot->GetName(), message);

            // Audible speech reaches bystander bots: they remember it and may
            // (config permitting) react to it.
            if (spokeAloud)
                Overhear::OnBotSpeech(context.bot, trigger, message, yelled);
            else if (spokeInChannel)
                Overhear::OnBotChannelSpeech(context.bot, trigger, message);

            return true;
        }

        // The next two helpers back both a tool's availability predicate (so
        // impossible actions are never offered to the model) and its executor
        // (state can change between prompt and execution).

        bool InviteBlocked(Player* bot, Player* actor, std::string& error)
        {
            if (bot->GetTeamId() != actor->GetTeamId())
            {
                error = "they are on the opposing faction";
                return true;
            }
            if (Group* group = bot->GetGroup())
            {
                if (actor->GetGroup() == group)
                {
                    error = "they are already in your group";
                    return true;
                }
                if (!group->IsLeader(bot->GetGUID()))
                {
                    error = "you are not the group leader";
                    return true;
                }
                if (group->IsFull())
                {
                    error = "your group is full";
                    return true;
                }
            }
            if (actor->GetGroup() || actor->GetGroupInvite())
            {
                error = "they are already in another group or have a pending invite";
                return true;
            }
            return false;
        }

        bool DuelBlocked(Player* bot, Player* actor, std::string& error)
        {
            if (!bot->IsAlive() || !actor->IsAlive())
            {
                error = "someone is dead";
                return true;
            }
            if (bot->duel || actor->duel)
            {
                error = "a duel is already pending";
                return true;
            }
            if (bot->IsInCombat() || actor->IsInCombat())
            {
                error = "someone is in combat";
                return true;
            }
            if (!bot->IsWithinDistInMap(actor, 10.0f))
            {
                error = "they are too far away";
                return true;
            }
            return false;
        }
    }

    std::string SanitizeChatText(std::string text)
    {
        // Cut anything that looks like leaked tool syntax.
        for (char const* marker : { "<tool_call>", "</tool_call>", "<|", "{\"name\":" })
        {
            size_t pos = text.find(marker);
            if (pos != std::string::npos)
                text.erase(pos);
        }

        // Trim whitespace and one layer of wrapping quotes.
        auto isTrimmable = [](unsigned char c) { return std::isspace(c) != 0; };
        while (!text.empty() && isTrimmable(text.front()))
            text.erase(text.begin());
        while (!text.empty() && isTrimmable(text.back()))
            text.pop_back();
        if (text.size() >= 2 && text.front() == '"' && text.back() == '"')
            text = text.substr(1, text.size() - 2);

        return text;
    }

    void RegisterDefaultTools()
    {
        // say - available on every trigger; audience bound by the trigger.
        sLlmToolRegistry->Register({
            "say",
            "Send a chat message to whoever you are currently talking with (the same channel the "
            "conversation is happening in).",
            {
                { "type", "object" },
                { "properties", { { "message", { { "type", "string" },
                    { "description", "The chat message to send, plain text" } } } } },
                { "required", { "message" } }
            },
            TRIGGER_ALL,
            false,
            [](ToolExecContext& context, nlohmann::json const& args, std::string& error)
            {
                std::string message = SanitizeChatText(args["message"].get<std::string>());
                if (message.empty())
                {
                    error = "empty message";
                    return false;
                }
                return RouteSay(context, message, error);
            }
        });

        // emote - visible social animation.
        {
            nlohmann::json emoteNames = nlohmann::json::array();
            for (std::string const& name : TextEmoteCatalog::AllNames())
                emoteNames.push_back(name);

            sLlmToolRegistry->Register({
                "emote",
                "Perform a visible emote animation, like a real player using /wave or /laugh.",
                {
                    { "type", "object" },
                    { "properties", { { "emote", { { "type", "string" }, { "enum", emoteNames } } } } },
                    { "required", { "emote" } }
                },
                TRIGGER_ALL,
                false,
                [](ToolExecContext& context, nlohmann::json const& args, std::string& error)
                {
                    uint32 emoteId = TextEmoteCatalog::FindId(args["emote"].get<std::string>());
                    if (!emoteId)
                    {
                        error = "unknown emote";
                        return false;
                    }
                    if (!context.ai->PlayEmote(emoteId))
                    {
                        error = "emote could not be performed";
                        return false;
                    }
                    return true;
                }
            });
        }

        // remember - upsert one note in the bot's private scratchpad.
        sLlmToolRegistry->Register({
            "remember",
            "Save a short private note to your memory; notes are shown back to you later. A note with "
            "the same slug is overwritten, so reuse a slug to update a note. Set about_player=true when "
            "the note is about the player you are interacting with, so it comes back whenever you meet "
            "them again. Use it for people you meet, promises, grudges, and plans.",
            {
                { "type", "object" },
                { "properties", {
                    { "slug", { { "type", "string" },
                        { "description", "Short id for the note, lowercase words joined by dashes" } } },
                    { "content", { { "type", "string" },
                        { "description", "The note itself, one or two short sentences" } } },
                    { "about_player", { { "type", "boolean" },
                        { "description", "True when the note is about the player you are interacting with" } } }
                } },
                { "required", { "slug", "content" } }
            },
            TRIGGER_ALL,
            false,
            [](ToolExecContext& context, nlohmann::json const& args, std::string& error)
            {
                if (!sLlmConfig->memoryEnabled)
                {
                    error = "memory is disabled";
                    return false;
                }

                uint32 subjectGuid = 0;
                if (args.value("about_player", false))
                {
                    if (!context.trigger->actorGuid)
                    {
                        error = "there is no player in this interaction to attach the note to";
                        return false;
                    }
                    subjectGuid = context.trigger->actorGuid.GetCounter();
                }

                error = sLlmMemoryStore->Upsert(context.trigger->botGuid,
                    args["slug"].get<std::string>(), subjectGuid, args["content"].get<std::string>());
                return error.empty();
            }
        });

        // forget - drop one note from the scratchpad.
        sLlmToolRegistry->Register({
            "forget",
            "Delete one of your private notes by its slug, for notes that are stale or no longer matter.",
            {
                { "type", "object" },
                { "properties", { { "slug", { { "type", "string" } } } } },
                { "required", { "slug" } }
            },
            TRIGGER_ALL,
            false,
            [](ToolExecContext& context, nlohmann::json const& args, std::string& error)
            {
                if (!sLlmConfig->memoryEnabled)
                {
                    error = "memory is disabled";
                    return false;
                }
                if (!sLlmMemoryStore->Forget(context.trigger->botGuid, args["slug"].get<std::string>()))
                {
                    error = "you have no note with that slug";
                    return false;
                }
                return true;
            }
        });

        // invite_to_party - synthetic client packet so all core validation runs.
        sLlmToolRegistry->Register({
            "invite_to_party",
            "Invite the player you are interacting with to join your party. Only use when they asked "
            "to group up or grouping clearly makes sense.",
            {
                { "type", "object" },
                { "properties", nlohmann::json::object() }
            },
            TRIGGER_CHAT_SAY | TRIGGER_CHAT_WHISPER | TRIGGER_CHAT_CHANNEL | TRIGGER_EMOTE | TRIGGER_GAME_EVENT,
            true,
            [](ToolExecContext& context, nlohmann::json const& /*args*/, std::string& error)
            {
                if (InviteBlocked(context.bot, context.actor, error))
                    return false;

                WorldPacket packet;
                packet << context.actor->GetName();
                packet << uint32(0); // roles mask
                context.bot->GetSession()->HandleGroupInviteOpcode(packet);
                return true;
            },
            [](Player* bot, Player* actor)
            {
                std::string ignored;
                return !InviteBlocked(bot, actor, ignored);
            }
        });

        // challenge_duel - casts the "Challenge to a Duel" spell; core validates.
        sLlmToolRegistry->Register({
            "challenge_duel",
            "Challenge the player you are interacting with to a duel. Only use when they asked for a "
            "duel or clearly provoked one.",
            {
                { "type", "object" },
                { "properties", nlohmann::json::object() }
            },
            TRIGGER_CHAT_SAY | TRIGGER_CHAT_WHISPER | TRIGGER_EMOTE,
            true,
            [](ToolExecContext& context, nlohmann::json const& /*args*/, std::string& error)
            {
                if (DuelBlocked(context.bot, context.actor, error))
                    return false;

                constexpr uint32 SPELL_DUEL_CHALLENGE = 7266;
                context.bot->CastSpell(context.actor, SPELL_DUEL_CHALLENGE, false);
                return true;
            },
            [](Player* bot, Player* actor)
            {
                std::string ignored;
                return !DuelBlocked(bot, actor, ignored);
            }
        });

        LOG_INFO("module.llm", "Registered default LLM tools");
    }
}
