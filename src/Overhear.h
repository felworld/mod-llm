/*
 * This file is part of mod-llm for Felworld. Released under the MIT license
 * (see the LICENSE file at the module root).
 */

#ifndef MOD_LLM_OVERHEAR_H
#define MOD_LLM_OVERHEAR_H

#include <string>

class Player;

namespace ModLlm
{
    struct TriggerContext;

    // Ambient hearing: a say/yell lands in the overheard transcript of every
    // bot in listen range (LLM.Chat.Overhear.Enable), and bot speech may in
    // turn trigger nearby bots to react - the latter kept behind its own
    // LLM.Chat.BotTrigger.* switches so bot-to-bot chatter can be flipped or
    // tuned independently of everything else. World thread only.
    namespace Overhear
    {
        // Record `message` in the overheard transcript of every bot in
        // earshot of `speaker`, and of the speaker itself when it is a bot
        // (bots must remember their own words to avoid repeating them).
        void RecordSpeech(Player* speaker, std::string const& message, bool yell);

        // A bot just said/yelled `message` (its reaction to sourceTrigger):
        // record it as overheard, then maybe trigger nearby bots to reply.
        void OnBotSpeech(Player* bot, TriggerContext const& sourceTrigger,
            std::string const& message, bool yell);

        // A bot just spoke in a named channel: maybe trigger other bots on
        // the channel (the room transcript already records the line). The
        // channel is passed explicitly because a say redirected via the
        // `destination` argument can land somewhere other than the trigger's
        // channel.
        void OnBotChannelSpeech(Player* bot, TriggerContext const& sourceTrigger,
            std::string const& channelName, std::string const& message);

        // A bot just whispered `receiver`: when the receiver is a bot too,
        // record the line in its pair history and maybe trigger a reply
        // (whispers between real players and bots flow through the chat
        // hook instead; bot-sent whispers bypass it).
        void OnBotWhisper(Player* bot, TriggerContext const& sourceTrigger,
            Player* receiver, std::string const& message);
    }
}

#endif
