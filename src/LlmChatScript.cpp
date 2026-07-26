/*
 * This file is part of mod-llm for Felworld. Released under the MIT license
 * (see the LICENSE file at the module root).
 */

#include "BotSelector.h"
#include "Channel.h"
#include "ChatHelper.h"
#include "Group.h"
#include "Guild.h"
#include "HistoryStore.h"
#include "LlmConfig.h"
#include "LlmDispatch.h"
#include "LlmRouter.h"
#include "ObjectAccessor.h"
#include "Overhear.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "PlayerbotMgr.h"
#include "Random.h"
#include "ScriptMgr.h"
#include "SharedDefines.h"
#include "StringFormat.h"
#include "TextEmoteCatalog.h"
#include "World.h"

#include <set>
#include <vector>

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

            bool female = player->getGender() == GENDER_FEMALE;

            // A bot that was emoted at directly reacts with high probability;
            // otherwise one random nearby bot may react.
            Player* target = guid.IsPlayer() ? ObjectAccessor::FindPlayer(guid) : nullptr;
            if (target && target != player && !BotSelector::IsRealPlayer(target))
            {
                // The bot reads exactly the line the client would show it.
                // No line (a sound/animation-only emote like /train) means
                // there is nothing to react to.
                std::string description = TextEmoteCatalog::Describe(textEmote,
                    TextEmoteCatalog::Target::You, female);
                if (description.empty())
                    return;

                if (urand(0, 99) >= sLlmConfig->emoteTargetedChance)
                    return;
                if (sLlmConfig->skipInCombat && target->IsInCombat())
                    return;

                TriggerContext trigger;
                trigger.kind = TRIGGER_EMOTE;
                trigger.message = std::move(description);
                MarkCrossFaction(trigger, target, player);
                Dispatch::Submit(target, player, std::move(trigger));
                return;
            }

            // A bystander sees the emote's target by name ("hugs Fluffy"),
            // or the untargeted line when there is no target.
            std::string description;
            Unit* targetUnit = guid ? ObjectAccessor::GetUnit(*player, guid) : nullptr;
            if (targetUnit && targetUnit != player)
                description = TextEmoteCatalog::Describe(textEmote,
                    TextEmoteCatalog::Target::Named, female, targetUnit->GetName());
            else
                description = TextEmoteCatalog::Describe(textEmote,
                    TextEmoteCatalog::Target::None, female);
            if (description.empty())
                return;

            if (urand(0, 99) >= sLlmConfig->emoteNearbyChance)
                return;

            for (Player* bot : BotSelector::SelectNearby(player, sLlmConfig->emoteDistance, 1, false))
            {
                TriggerContext trigger;
                trigger.kind = TRIGGER_EMOTE;
                trigger.message = description;
                MarkCrossFaction(trigger, bot, player);
                Dispatch::Submit(bot, player, std::move(trigger));
            }
        }

    private:
        // Emotes cross the faction line even though language does not: the
        // bot may react, but chatting back only works out (as deliberate
        // gibberish) when the cross-faction dice allow it.
        static void MarkCrossFaction(TriggerContext& trigger, Player* bot, Player* actor)
        {
            if (BotSelector::CanUnderstand(bot, actor))
                return;
            trigger.crossFaction = true;
            trigger.crossFactionChatOk = urand(0, 99) < sLlmConfig->crossFactionChatChance;
        }

        void HandleChat(Player* sender, uint32 type, uint32 lang, std::string const& msg,
            Player* receiver, Group* group, Guild* guild, Channel* channel)
        {
            if (!sLlmConfig->IsEnabled() || !sLlmConfig->chatEnabled)
                return;
            if (lang == LANG_ADDON || msg.empty())
                return;

            // A message starting with AiPlayerbot.CommandPrefix is a bot
            // command (mod-playerbots executes it), not conversation: no
            // replies, and it stays out of every transcript.
            std::string const& commandPrefix = sPlayerbotAIConfig.commandPrefix;
            if (!commandPrefix.empty() && msg.rfind(commandPrefix, 0) == 0)
                return;

            // Chat links arrive as raw client markup: pull the linked quest
            // ids out for routing (a candidate on the quest is a natural
            // responder), then reduce the message to the text a player sees.
            std::set<uint32> questIds = ChatHelper::ExtractAllQuestIds(msg);
            std::vector<uint32> linkedQuests(questIds.begin(), questIds.end());
            std::string text = BotSelector::NormalizeChatLinks(msg);

            // Whispers: only the addressed bot may react, and always does.
            if (receiver)
            {
                if (!sLlmConfig->whispersEnabled || BotSelector::IsRealPlayer(receiver))
                    return;
                // Cross-faction whispers cannot happen for real players (GMs
                // excepted); drop the ones bots produce by writing to the
                // session directly.
                if (!sWorld->getBoolConfig(CONFIG_ALLOW_TWO_SIDE_INTERACTION_CHAT)
                    && sender->GetTeamId() != receiver->GetTeamId() && !sender->IsGameMaster())
                    return;
                if (sLlmConfig->skipInCombat && receiver->IsInCombat())
                    return;

                sLlmHistoryStore->AddPairLine(receiver->GetGUID(), sender->GetGUID(), false,
                    sender->GetName(), text);

                TriggerContext trigger;
                trigger.kind = TRIGGER_CHAT_WHISPER;
                trigger.chatType = type;
                trigger.message = text;
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

                // Every bot in earshot remembers the line, reacting or not.
                Overhear::RecordSpeech(sender, text, type == CHAT_MSG_YELL);
            }

            if (!roomKey.empty())
                sLlmHistoryStore->AddRoomLine(roomKey, sender->GetGUID(), sender->GetName(), text);

            // Raid and battleground messages from a real player go through
            // the router: one cheap LLM call picks which bots the message is
            // for (name, class, and role considered) instead of random dice.
            if (group && sLlmConfig->groupRouterEnabled && BotSelector::IsRealPlayer(sender)
                && (group->isRaidGroup() || group->isBGGroup() || group->isBFGroup()))
            {
                std::vector<Player*> candidates = BotSelector::CollectGroupBots(sender, group);
                if (candidates.empty())
                    return;

                TriggerContext trigger;
                trigger.kind = kind;
                trigger.chatType = type;
                trigger.roomKey = roomKey;
                trigger.message = text;
                trigger.linkedQuests = linkedQuests;
                Router::RouteGroupMessage(sender, candidates, std::move(trigger));
                return;
            }

            // A real player's say/yell likewise goes through the router: with
            // the recently-overheard conversation as context, one cheap LLM
            // call picks whoever the message is actually meant for (or
            // nobody) - so an undirected reply reaches the bot the player is
            // talking to instead of rolling dice across bystanders.
            if (kind == TRIGGER_CHAT_SAY && sLlmConfig->sayRouterEnabled && BotSelector::IsRealPlayer(sender))
            {
                std::vector<Player*> candidates = BotSelector::CollectSayCandidates(sender, maxDistance);
                if (candidates.empty())
                    return;

                TriggerContext trigger;
                trigger.kind = kind;
                trigger.chatType = type;
                trigger.message = text;
                trigger.linkedQuests = linkedQuests;
                Router::RouteSayMessage(sender, candidates, std::move(trigger));
                return;
            }

            // Guild and named-channel messages route the same way, over the
            // room transcript - defense channels included: their read-mostly
            // feel comes from the router judging that most alarms are
            // answered by nobody, which per-candidate dice cannot do on a
            // faction-wide channel.
            if (sLlmConfig->roomRouterEnabled && BotSelector::IsRealPlayer(sender)
                && (kind == TRIGGER_CHAT_GUILD || kind == TRIGGER_CHAT_CHANNEL))
            {
                std::vector<Player*> candidates = kind == TRIGGER_CHAT_GUILD
                    ? BotSelector::CollectGuildBots(sender, guild)
                    : BotSelector::CollectChannelBots(sender, channel);
                if (candidates.empty())
                    return;

                TriggerContext trigger;
                trigger.kind = kind;
                trigger.chatType = type;
                trigger.roomKey = roomKey;
                trigger.message = text;
                trigger.linkedQuests = linkedQuests;
                if (channel)
                {
                    trigger.channelName = channel->GetName();
                    trigger.defenseChannel = BotSelector::IsDefenseChannel(channel);
                }

                Router::RouteRoomMessage(sender, candidates, std::move(trigger));
                return;
            }

            std::vector<Player*> bots = BotSelector::SelectForChat(sender, kind, text, group, guild,
                channel, maxDistance);

            // Successive responders are staggered so each one's context is
            // rebuilt after the previous replies (likely) landed - concurrent
            // requests would otherwise see identical history and converge on
            // near-identical answers.
            uint32 staggerMs = sLlmConfig->chatStaggerSeconds * IN_MILLISECONDS;
            uint32 index = 0;

            for (Player* bot : bots)
            {
                // Direct exchanges (say/yell) also feed the pair transcript.
                if (kind == TRIGGER_CHAT_SAY)
                    sLlmHistoryStore->AddPairLine(bot->GetGUID(), sender->GetGUID(), false,
                        sender->GetName(), text);

                TriggerContext trigger;
                trigger.kind = kind;
                trigger.chatType = type;
                trigger.roomKey = roomKey;
                trigger.message = text;
                trigger.linkedQuests = linkedQuests;
                if (channel)
                {
                    trigger.channelName = channel->GetName();
                    trigger.defenseChannel = BotSelector::IsDefenseChannel(channel);
                }

                if (uint32 delayMs = index * staggerMs)
                    Dispatch::SubmitDelayed(bot, sender, std::move(trigger), delayMs);
                else
                    Dispatch::Submit(bot, sender, std::move(trigger));
                ++index;
            }
        }
    };
}

void AddSC_llm_chat()
{
    new ModLlm::LlmChatScript();
}
