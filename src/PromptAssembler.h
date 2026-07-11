/*
 * This file is part of mod-llm for Felworld. Released under the MIT license
 * (see the LICENSE file at the module root).
 */

#ifndef MOD_LLM_PROMPT_ASSEMBLER_H
#define MOD_LLM_PROMPT_ASSEMBLER_H

#include "ContextBuilder.h"
#include "LlmTrigger.h"

#include <nlohmann/json.hpp>

namespace ModLlm::PromptAssembler
{
    // Renders the OpenAI chat-completions "messages" array (system + user)
    // from a snapshot. Pure function of its inputs and the config templates;
    // safe on any thread. Falls back to built-in templates if a configured
    // template has bad placeholders.
    nlohmann::json BuildMessages(ContextSnapshot const& snapshot, TriggerContext const& trigger);
}

#endif
