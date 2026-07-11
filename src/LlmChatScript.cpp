/*
 * This file is part of mod-llm for Felworld. Released under the MIT license
 * (see the LICENSE file at the module root).
 */

#include "BotSelector.h"
#include "Channel.h"
#include "Group.h"
#include "Guild.h"
#include "HistoryStore.h"
#include "LlmConfig.h"
#include "LlmDispatch.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotMgr.h"
#include "Random.h"
#include "ScriptMgr.h"
#include "SharedDefines.h"
#include "StringFormat.h"
#include "TextEmoteCatalog.h"

namespace ModLlm
{
    // Reactive triggers: player chat (all five audiences) and text emotes.
    // These hooks run on the world thread (session updates).
    class LlmChatScript : public PlayerScript
    {
    public:
        LlmChatScript() : PlayerScript("LlmChatScript", {
            PLAYERHOOK_CAN_PLAYER_USE_CHAT,
            PLAYERHOOK_CAN_PLAYER_USE_PRIVATE_CHAT,
            PLAYERHOOK_CAN_PLAYER_USE_GROUP_CHAT,
            PLAYERHOOK_CAN_PLAYER_USE_GUILD_CHAT,
            PLAYERHOOK_CAN_PLAYER_USE_CHANNEL_CHAT,
            PLAYERHOOK_ON_TEXT_EMOTE
        }) { }

        bool OnPlayerCanUseChat(Player* player, uint32 type, uint32 lang, std::string& msg) override
        {
            HandleChat(player, type, lang, msg, nullptr, nullptr, nullptr, nullptr);
            return true;
        }

        bool OnPlayerCanUseChat(Player* player, uint32 type, uint32 lang, std::string& msg, Player* receiver) override
        {
            HandleChat(player, type, lang, msg, receiver, nullptr, nullptr, nullptr);
            return true;
        }

        bool OnPlayerCanUseChat(Player* player, uint32 type, uint32 lang, std::string& msg, Group* group) override
        {
            HandleChat(player, type, lang, msg, nullptr, group, nullptr, nullptr);
            return true;
        }

        bool OnPlayerCanUseChat(Player* player, uint32 type, uint32 lang, std::string& msg, Guild* guild) override
        {
            HandleChat(player, type, lang, msg, nullptr, nullptr, guild, nullptr);
            return true;
        }

        bool OnPlayerCanUseChat(Player* player, uint32 type, uint32 lang, std::string& msg, Channel* channel) override
        {
            HandleChat(player, type, lang, msg, nullptr, nullptr, nullptr, channel);
            return true;
        }

        void OnPlayerTextEmote(Player* player, uint32 textEmote, uint32 /*emoteNum*/, ObjectGuid guid) override
        {
            if (!sLlmConfig->IsEnabled() || !sLlmConfig->emoteEnabled)
                return;

            std::string emoteName = TextEmoteCatalog::FindName(textEmote);
            if (emoteName.empty())
                emoteName = "an emote";

            // A bot that was emoted at directly reacts with high probability;
            // otherwise one random nearby bot may react.
            Player* target = guid.IsPlayer() ? ObjectAccessor::FindPlayer(guid) : nullptr;
            if (target && target != player && !BotSelector::IsRealPlayer(target))
            {
                if (urand(0, 99) >= sLlmConfig->emoteTargetedChance)
                    return;
                if (sLlmConfig->skipInCombat && target->IsInCombat())
                    return;

                TriggerContext trigger;
                trigger.kind = TRIGGER_EMOTE;
                trigger.message = Acore::StringFormat("performs the emote \"{}\" directed at you", emoteName);
                Dispatch::Submit(target, player, std::move(trigger));
                return;
            }

            if (urand(0, 99) >= sLlmConfig->emoteNearbyChance)
                return;

            for (Player* bot : BotSelector::SelectNearby(player, sLlmConfig->emoteDistance, 1, false))
            {
                TriggerContext trigger;
                trigger.kind = TRIGGER_EMOTE;
                trigger.message = Acore::StringFormat("performs the emote \"{}\" nearby", emoteName);
                Dispatch::Submit(bot, player, std::move(trigger));
            }
        }

    private:
        void HandleChat(Player* sender, uint32 type, uint32 lang, std::string const& msg,
            Player* receiver, Group* group, Guild* guild, Channel* channel)
        {
            if (!sLlmConfig->IsEnabled() || !sLlmConfig->chatEnabled)
                return;
            if (lang == LANG_ADDON || msg.empty())
                return;

            // Whispers: only the addressed bot may react, and always does.
            if (receiver)
            {
                if (!sLlmConfig->whispersEnabled || BotSelector::IsRealPlayer(receiver))
                    return;
                if (sLlmConfig->skipInCombat && receiver->IsInCombat())
                    return;

                sLlmHistoryStore->AddPairLine(receiver->GetGUID(), sender->GetGUID(), false,
                    sender->GetName(), msg);

                TriggerContext trigger;
                trigger.kind = TRIGGER_CHAT_WHISPER;
                trigger.chatType = type;
                trigger.message = msg;
                Dispatch::Submit(receiver, sender, std::move(trigger));
                return;
            }

            uint32 kind;
            std::string roomKey;
            float maxDistance = sLlmConfig->sayDistance;

            if (group)
            {
                kind = TRIGGER_CHAT_PARTY;
                roomKey = Acore::StringFormat("group:{}", group->GetGUID().GetCounter());
            }
            else if (guild)
            {
                kind = TRIGGER_CHAT_GUILD;
                roomKey = Acore::StringFormat("guild:{}", guild->GetId());
            }
            else if (channel)
            {
                if (!channel->GetChannelId() && !sLlmConfig->customChannelsEnabled)
                    return;
                kind = TRIGGER_CHAT_CHANNEL;
                roomKey = Acore::StringFormat("channel:{}:{}", channel->GetName(), uint32(sender->GetTeamId()));
            }
            else
            {
                kind = TRIGGER_CHAT_SAY;
                if (type == CHAT_MSG_YELL)
                    maxDistance = sLlmConfig->yellDistance;
            }

            if (!roomKey.empty())
                sLlmHistoryStore->AddRoomLine(roomKey, sender->GetGUID(), sender->GetName(), msg);

            std::vector<Player*> bots = BotSelector::SelectForChat(sender, kind, msg, group, guild,
                channel, maxDistance);

            for (Player* bot : bots)
            {
                // Direct exchanges (say/yell) also feed the pair transcript.
                if (kind == TRIGGER_CHAT_SAY)
                    sLlmHistoryStore->AddPairLine(bot->GetGUID(), sender->GetGUID(), false,
                        sender->GetName(), msg);

                TriggerContext trigger;
                trigger.kind = kind;
                trigger.chatType = type;
                trigger.roomKey = roomKey;
                trigger.message = msg;
                if (channel)
                {
                    trigger.channelName = channel->GetName();
                    trigger.channelId = channel->GetChannelId();
                }
                Dispatch::Submit(bot, sender, std::move(trigger));
            }
        }
    };
}

void AddSC_llm_chat()
{
    new ModLlm::LlmChatScript();
}
