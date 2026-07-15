/*
 * This file is part of mod-llm for Felworld. Released under the MIT license
 * (see the LICENSE file at the module root).
 */

#include "Overhear.h"

#include "BotSelector.h"
#include "Channel.h"
#include "ChannelMgr.h"
#include "Common.h"
#include "HistoryStore.h"
#include "LlmConfig.h"
#include "LlmDispatch.h"
#include "LlmTrigger.h"
#include "Player.h"
#include "SharedDefines.h"

namespace ModLlm::Overhear
{
    namespace
    {
        bool ChainAllowed(TriggerContext const& sourceTrigger)
        {
            return sLlmConfig->chatEnabled && sLlmConfig->botTriggerEnabled
                && sourceTrigger.chainDepth < sLlmConfig->botTriggerMaxChainDepth;
        }

        // Same stagger as the reactive chat path: each later responder sees
        // the replies that landed before its turn.
        void SubmitStaggered(std::vector<Player*> const& bots, Player* speaker, TriggerContext trigger)
        {
            uint32 staggerMs = sLlmConfig->chatStaggerSeconds * IN_MILLISECONDS;
            uint32 index = 0;

            for (Player* bot : bots)
            {
                TriggerContext copy = trigger;
                Dispatch::SubmitDelayed(bot, speaker, std::move(copy), index * staggerMs);
                ++index;
            }
        }
    }

    void RecordSpeech(Player* speaker, std::string const& message, bool yell)
    {
        if (!sLlmConfig->overhearEnabled || !sLlmConfig->historyEnabled)
            return;

        float distance = yell ? sLlmConfig->yellDistance : sLlmConfig->sayDistance;
        for (Player* bot : BotSelector::CollectListeners(speaker, distance))
            sLlmHistoryStore->AddOverheardLine(bot->GetGUID(), speaker->GetName(), message);

        if (!BotSelector::IsRealPlayer(speaker))
            sLlmHistoryStore->AddOverheardLine(speaker->GetGUID(), speaker->GetName(), message);
    }

    void OnBotSpeech(Player* bot, TriggerContext const& sourceTrigger,
        std::string const& message, bool yell)
    {
        RecordSpeech(bot, message, yell);

        if (!ChainAllowed(sourceTrigger))
            return;

        float distance = yell ? sLlmConfig->yellDistance : sLlmConfig->sayDistance;
        std::vector<Player*> bots = BotSelector::SelectForChat(bot, TRIGGER_CHAT_SAY, message,
            nullptr, nullptr, nullptr, distance);

        for (Player* other : bots)
            sLlmHistoryStore->AddPairLine(other->GetGUID(), bot->GetGUID(), false,
                bot->GetName(), message);

        TriggerContext trigger;
        trigger.kind = TRIGGER_CHAT_SAY;
        trigger.chatType = yell ? CHAT_MSG_YELL : CHAT_MSG_SAY;
        trigger.message = message;
        trigger.chainDepth = sourceTrigger.chainDepth + 1;
        SubmitStaggered(bots, bot, std::move(trigger));
    }

    void OnBotChannelSpeech(Player* bot, TriggerContext const& sourceTrigger,
        std::string const& message)
    {
        if (!ChainAllowed(sourceTrigger) || sourceTrigger.channelName.empty())
            return;

        ChannelMgr* mgr = ChannelMgr::forTeam(bot->GetTeamId());
        Channel* channel = mgr ? mgr->GetChannel(sourceTrigger.channelName, bot, false) : nullptr;
        if (!channel)
            return;

        std::vector<Player*> bots = BotSelector::SelectForChat(bot, TRIGGER_CHAT_CHANNEL, message,
            nullptr, nullptr, channel, 0.0f);

        TriggerContext trigger;
        trigger.kind = TRIGGER_CHAT_CHANNEL;
        trigger.chatType = CHAT_MSG_CHANNEL;
        trigger.channelName = sourceTrigger.channelName;
        trigger.roomKey = sourceTrigger.roomKey;
        trigger.message = message;
        trigger.chainDepth = sourceTrigger.chainDepth + 1;
        SubmitStaggered(bots, bot, std::move(trigger));
    }
}
