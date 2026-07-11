/*
 * This file is part of mod-llm for Felworld. Released under the MIT license
 * (see the LICENSE file at the module root).
 */

#ifndef MOD_LLM_DISPATCH_H
#define MOD_LLM_DISPATCH_H

#include "LlmTrigger.h"

class Player;

namespace ModLlm::Dispatch
{
    // Fills in the trigger's identity fields, snapshots the game state on the
    // calling thread, and enqueues the LLM request. `actor` may be nullptr.
    bool Submit(Player* bot, Player* actor, TriggerContext trigger);
}

#endif
