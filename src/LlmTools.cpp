/*
 * This file is part of mod-llm for Felworld. Released under the MIT license
 * (see the LICENSE file at the module root).
 */

#include "LlmTools.h"

#include "Channel.h"
#include "ChannelMgr.h"
#include "Group.h"
#include "HistoryStore.h"
#include "LlmConfig.h"
#include "LlmTrigger.h"
#include "Log.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "SentimentStore.h"
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
        bool IsBuiltinChannelId(uint32 id)
        {
            switch (id)
            {
                case GENERAL:
                case TRADE:
                case LOCAL_DEFENSE:
                case WORLD_DEFENSE:
                case GUILD_RECRUITMENT:
                case LOOKING_FOR_GROUP:
                    return true;
                default:
                    return false;
            }
        }

        // Sends `message` back into the channel the trigger came from. The
        // audience is bound by the trigger, never chosen by the model.
        bool RouteSay(ToolExecContext& context, std::string const& message, std::string& error)
        {
            TriggerContext const& trigger = *context.trigger;
            bool sent = false;

            switch (trigger.kind)
            {
                case TRIGGER_CHAT_WHISPER:
                    sent = context.ai->Whisper(message, trigger.actorName);
                    break;
                case TRIGGER_CHAT_PARTY:
                    if (trigger.chatType == CHAT_MSG_RAID || trigger.chatType == CHAT_MSG_RAID_LEADER
                        || trigger.chatType == CHAT_MSG_RAID_WARNING)
                        sent = context.ai->SayToRaid(message);
                    else
                        sent = context.ai->SayToParty(message);
                    break;
                case TRIGGER_CHAT_GUILD:
                    sent = context.ai->SayToGuild(message);
                    break;
                case TRIGGER_CHAT_CHANNEL:
                    if (IsBuiltinChannelId(trigger.channelId))
                        sent = context.ai->SayToChannel(message, ChatChannelId(trigger.channelId));
                    else if (ChannelMgr* mgr = ChannelMgr::forTeam(context.bot->GetTeamId()))
                    {
                        if (Channel* channel = mgr->GetChannel(trigger.channelName, context.bot, false))
                        {
                            if (channel->IsOn(context.bot->GetGUID()))
                            {
                                channel->Say(context.bot->GetGUID(), message, LANG_UNIVERSAL);
                                sent = true;
                            }
                        }
                    }
                    break;
                default:
                    if (trigger.chatType == CHAT_MSG_YELL)
                        sent = context.ai->Yell(message);
                    else
                        sent = context.ai->Say(message);
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

            return true;
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

        // adjust_sentiment - the bot's lasting opinion of the actor.
        sLlmToolRegistry->Register({
            "adjust_sentiment",
            "Adjust your lasting opinion of the player you are interacting with. Use when they are "
            "notably friendly, helpful, rude, or hostile toward you.",
            {
                { "type", "object" },
                { "properties", {
                    { "direction", { { "type", "string" }, { "enum", { "up", "down" } } } },
                    { "intensity", { { "type", "string" }, { "enum", { "slight", "strong" } } } }
                } },
                { "required", { "direction" } }
            },
            TRIGGER_ALL,
            true,
            [](ToolExecContext& context, nlohmann::json const& args, std::string& error)
            {
                if (!sLlmConfig->sentimentEnabled)
                {
                    error = "sentiment tracking is disabled";
                    return false;
                }

                bool up = args["direction"].get<std::string>() == "up";
                bool strong = args.value("intensity", "slight") == "strong";
                float step = strong ? sLlmConfig->sentimentStepLarge : sLlmConfig->sentimentStepSmall;
                sLlmSentimentStore->Adjust(context.trigger->botGuid, context.trigger->actorGuid,
                    up ? step : -step);
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
                Player* bot = context.bot;
                Player* actor = context.actor;

                if (bot->GetTeamId() != actor->GetTeamId())
                {
                    error = "actor is on the opposing faction";
                    return false;
                }
                if (actor->GetGroup() || actor->GetGroupInvite())
                {
                    error = "actor is already grouped or has a pending invite";
                    return false;
                }
                if (Group* group = bot->GetGroup())
                {
                    if (!group->IsLeader(bot->GetGUID()))
                    {
                        error = "bot is not the group leader";
                        return false;
                    }
                    if (group->IsFull())
                    {
                        error = "group is full";
                        return false;
                    }
                }

                WorldPacket packet;
                packet << actor->GetName();
                packet << uint32(0); // roles mask
                bot->GetSession()->HandleGroupInviteOpcode(packet);
                return true;
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
                Player* bot = context.bot;
                Player* actor = context.actor;

                if (!bot->IsAlive() || !actor->IsAlive())
                {
                    error = "someone is dead";
                    return false;
                }
                if (bot->duel || actor->duel)
                {
                    error = "a duel is already pending";
                    return false;
                }
                if (bot->IsInCombat() || actor->IsInCombat())
                {
                    error = "someone is in combat";
                    return false;
                }
                if (!bot->IsWithinDistInMap(actor, 10.0f))
                {
                    error = "actor is too far away";
                    return false;
                }

                constexpr uint32 SPELL_DUEL_CHALLENGE = 7266;
                bot->CastSpell(actor, SPELL_DUEL_CHALLENGE, false);
                return true;
            }
        });

        LOG_INFO("module.llm", "Registered default LLM tools");
    }
}
