/*
 * This file is part of mod-llm for Felworld. Released under the MIT license
 * (see the LICENSE file at the module root).
 */

#ifndef MOD_LLM_TOOLS_H
#define MOD_LLM_TOOLS_H

#include <string>

namespace ModLlm
{
    struct ToolExecContext;

    namespace LlmTools
    {
        // Registers the built-in tools: say, emote, remember, forget,
        // invite_to_party, challenge_duel, roll, leave_party, follow_player,
        // stop_following, buff_player, guild_invite, travel_to. Called once
        // at startup.
        void RegisterDefaultTools();

        // Executes travel_to trips whose bot is out of every human's sight.
        // Call periodically from the world update. World thread only.
        void UpdateTravel();

        // Removes quotation wrapping and tool-syntax artifacts that weak
        // models sometimes leak into chat text. Exposed for tests.
        std::string SanitizeChatText(std::string text);
    }
}

#endif
