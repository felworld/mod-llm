/*
 * This file is part of mod-llm for Felworld. Released under the MIT license
 * (see the LICENSE file at the module root).
 */

#ifndef MOD_LLM_DISPATCH_H
#define MOD_LLM_DISPATCH_H

#include "LlmTrigger.h"

#include "ObjectGuid.h"

#include <memory>

class Player;
class PlayerbotOperation;

namespace ModLlm::Dispatch
{
    // Fills in the trigger's identity fields, snapshots the game state on the
    // calling thread, and enqueues the LLM request. `actor` may be nullptr.
    bool Submit(Player* bot, Player* actor, TriggerContext trigger);

    // Like Submit, but fires after `delayMs`. The snapshot is built at fire
    // time, so the bot sees replies other bots produced in the meantime.
    // Safe from any thread; bot and actor are resolved when the delay ends.
    void SubmitDelayed(Player* bot, Player* actor, TriggerContext trigger, uint32 delayMs);

    // Same, for callers that only hold GUIDs (e.g. the group-chat router's
    // HTTP-worker callback). trigger.actorGuid/actorName must already be set.
    void SubmitDelayed(ObjectGuid botGuid, TriggerContext trigger, uint32 delayMs);

    // Holds a ready world-thread operation for `delayMs` before queueing it
    // on the PlayerbotWorldThreadProcessor - the faux-typing hold on a
    // generated reply. Operations for the same bot never overtake each
    // other: a second reply waits until the first has been "typed".
    // Safe from any thread.
    void QueueOperationDelayed(std::unique_ptr<PlayerbotOperation> operation, uint32 delayMs);

    // Drives the delayed queues; call every world update.
    void UpdateDelayed(uint32 diff);

    // Drops all pending delayed dispatches and held operations (module
    // disabled).
    void ClearDelayed();
}

#endif
