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
#include "LlmRouter.h"
#include "LlmTrigger.h"
#include "Player.h"
#include "SharedDefines.h"
#include "StringFormat.h"

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
        // The say itself already passed through LlmChatScript's chat hook
        // (Player::Say fires it for bots too), which ran RecordSpeech for
        // everyone in earshot - this function only decides who may reply.

        if (!ChainAllowed(sourceTrigger))
            return;

        float distance = yell ? sLlmConfig->yellDistance : sLlmConfig->sayDistance;

        TriggerContext trigger;
        trigger.kind = TRIGGER_CHAT_SAY;
        trigger.chatType = yell ? CHAT_MSG_YELL : CHAT_MSG_SAY;
        trigger.message = message;

        // An event comment is a coda, not an opener: a duelist's "gg" may
        // draw one answering line, but chains of acknowledgements ("gg" ->
        // "gl" -> "np") spiraled endlessly at duel hotspots
        // (felworld/mod-llm#22). Starting the chain at its cap leaves
        // exactly one reply hop.
        trigger.chainDepth = sourceTrigger.kind == TRIGGER_GAME_EVENT
            ? sLlmConfig->botTriggerMaxChainDepth
            : sourceTrigger.chainDepth + 1;

        // A bot's say routes exactly like a real player's: one cheap LLM
        // call, seeing the roster and the recent conversation, judges who -
        // if anyone - the line is for. Judgment instead of dice keeps
        // ambient exchanges alive without flat percentages compounding into
        // reply storms.
        if (sLlmConfig->sayRouterEnabled)
        {
            std::vector<Player*> candidates = BotSelector::CollectSayCandidates(bot, distance);
            if (!candidates.empty())
                Router::RouteSayMessage(bot, candidates, std::move(trigger));
            return;
        }

        std::vector<Player*> bots = BotSelector::SelectForChat(bot, TRIGGER_CHAT_SAY, message,
            nullptr, nullptr, nullptr, distance);

        for (Player* other : bots)
            sLlmHistoryStore->AddPairLine(other->GetGUID(), bot->GetGUID(), false,
                bot->GetName(), message);

        SubmitStaggered(bots, bot, std::move(trigger));
    }

    void OnBotChannelSpeech(Player* bot, TriggerContext const& sourceTrigger,
        std::string const& channelName, std::string const& message)
    {
        if (!ChainAllowed(sourceTrigger) || channelName.empty())
            return;

        ChannelMgr* mgr = ChannelMgr::forTeam(bot->GetTeamId());
        Channel* channel = mgr ? mgr->GetChannel(channelName, bot, false) : nullptr;
        if (!channel)
            return;

        TriggerContext trigger;
        trigger.kind = TRIGGER_CHAT_CHANNEL;
        trigger.chatType = CHAT_MSG_CHANNEL;
        trigger.channelName = channelName;
        trigger.defenseChannel = BotSelector::IsDefenseChannel(channel);
        trigger.roomKey = Acore::StringFormat("channel:{}:{}", channelName, uint32(bot->GetTeamId()));
        trigger.message = message;

        // Same event-comment coda rule as OnBotSpeech: one reply hop.
        trigger.chainDepth = sourceTrigger.kind == TRIGGER_GAME_EVENT
            ? sLlmConfig->botTriggerMaxChainDepth
            : sourceTrigger.chainDepth + 1;

        // Channel messages get the same routing judgment as say (the room
        // transcript stands in for the overheard conversation). Defense
        // channels included: per-candidate dice scale with the audience, so
        // on a faction-wide channel even a low chance answers every message
        // - their read-mostly feel comes from the router judging that most
        // alarms are answered by nobody.
        if (sLlmConfig->roomRouterEnabled)
        {
            std::vector<Player*> candidates = BotSelector::CollectChannelBots(bot, channel);
            if (!candidates.empty())
                Router::RouteRoomMessage(bot, candidates, std::move(trigger));
            return;
        }

        std::vector<Player*> bots = BotSelector::SelectForChat(bot, TRIGGER_CHAT_CHANNEL, message,
            nullptr, nullptr, channel, 0.0f);
        SubmitStaggered(bots, bot, std::move(trigger));
    }

    void OnBotWhisper(Player* bot, TriggerContext const& sourceTrigger,
        Player* receiver, std::string const& message)
    {
        if (BotSelector::IsRealPlayer(receiver))
            return;

        // The receiving bot remembers the whisper whether or not it replies.
        sLlmHistoryStore->AddPairLine(receiver->GetGUID(), bot->GetGUID(), false,
            bot->GetName(), message);

        if (!ChainAllowed(sourceTrigger) || !sLlmConfig->whispersEnabled)
            return;
        if (sLlmConfig->skipInCombat && receiver->IsInCombat())
            return;

        TriggerContext trigger;
        trigger.kind = TRIGGER_CHAT_WHISPER;
        trigger.chatType = CHAT_MSG_WHISPER;
        trigger.message = message;
        trigger.chainDepth = sourceTrigger.chainDepth + 1;
        Dispatch::Submit(receiver, bot, std::move(trigger));
    }
}
