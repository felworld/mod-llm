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
        // invite_to_party, challenge_duel. Called once at startup.
        void RegisterDefaultTools();

        // Removes quotation wrapping and tool-syntax artifacts that weak
        // models sometimes leak into chat text. Exposed for tests.
        std::string SanitizeChatText(std::string text);
    }
}

#endif
