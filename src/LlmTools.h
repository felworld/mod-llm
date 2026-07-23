/*
 * This file is part of mod-llm for Felworld. Released under the MIT license
 * (see the LICENSE file at the module root).
 */

#ifndef MOD_LLM_TOOLS_H
#define MOD_LLM_TOOLS_H

#include "Define.h"

#include <functional>
#include <string>

namespace ModLlm
{
    struct ToolExecContext;

    namespace LlmTools
    {
        // Registers the built-in tools: say, emote, remember, forget,
        // get_gear, get_inventory, invite_to_party, challenge_duel, roll,
        // leave_party, follow_player, stop_following, buff_player,
        // guild_invite, travel_to. Called once at startup.
        void RegisterDefaultTools();

        // Executes travel_to trips whose bot is out of every human's sight.
        // Call periodically from the world update. World thread only.
        void UpdateTravel();

        // Removes quotation wrapping and tool-syntax artifacts that weak
        // models sometimes leak into chat text. Exposed for tests.
        std::string SanitizeChatText(std::string text);

        // Replaces {item:ID}, {quest:ID} and {spell:ID} tags with clickable
        // client hyperlinks; a tag whose ID resolves to nothing is dropped so
        // a hallucinated ID can never reach chat as a broken link. Braces
        // that are not exactly a link tag pass through untouched.
        std::string ExpandChatLinks(std::string const& text);

        // Resolver-injected variant. Exposed for tests: `resolve` returns the
        // replacement for one tag, or an empty string to drop it.
        std::string ExpandChatLinks(std::string const& text,
            std::function<std::string(std::string const& kind, uint32 id)> const& resolve);
    }
}

#endif
