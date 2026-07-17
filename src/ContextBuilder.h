/*
 * This file is part of mod-llm for Felworld. Released under the MIT license
 * (see the LICENSE file at the module root).
 */

#ifndef MOD_LLM_CONTEXT_BUILDER_H
#define MOD_LLM_CONTEXT_BUILDER_H

#include "LlmTrigger.h"

#include <string>

class Player;

namespace ModLlm
{
    // Everything the prompt needs, captured as plain values on the hook thread.
    // After a ContextSnapshot is built, no game pointers cross into the async
    // pipeline.
    struct ContextSnapshot
    {
        std::string botName;
        uint32 botLevel = 0;
        std::string botClass;
        std::string botRace;
        std::string botFaction;
        std::string botArea;
        std::string botZone;
        std::string botGroup;   // "" or "You are in a party with X (leader), Y. "
        std::string botGuild;   // "" or "You are a member of the guild <X>. "

        std::string actorName;
        uint32 actorLevel = 0;
        std::string actorClass;
        std::string actorRace;

        std::string memoryBlock;      // preformatted "- [slug] content" note lines, may be empty
        std::string pairHistory;      // preformatted transcript lines, may be empty
        std::string roomHistory;
        std::string overheardHistory; // say/yell the bot witnessed nearby
        std::string channelLabel;     // "say", "party", "guild - <name>", channel name, ...
        std::string replyGuidance;    // audience reminder (group heard it / zone channel)
        std::string environment;   // initiative triggers only
    };

    namespace ContextBuilder
    {
        // bot must be valid; actor may be nullptr (name falls back to
        // trigger.actorName). Call on the thread that owns the objects.
        ContextSnapshot Build(Player* bot, Player* actor, TriggerContext const& trigger);

        // Short description of the bot's surroundings for initiative prompts.
        std::string DescribeEnvironment(Player* bot);
    }
}

#endif
