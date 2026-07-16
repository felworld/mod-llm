/*
 * This file is part of mod-llm for Felworld. Released under the MIT license
 * (see the LICENSE file at the module root).
 */

#ifndef MOD_LLM_ROUTER_H
#define MOD_LLM_ROUTER_H

#include "LlmTrigger.h"

#include <optional>
#include <string>
#include <vector>

class Player;

namespace ModLlm::Router
{
    // Raid and battleground chat reaches too many bots to ask them all, and
    // random picks miss the one bot a message is for ("any mages got
    // water?"). Instead, one cheap routing request shows the model the
    // message plus a roster of candidate bots (name, class, spec, role) and
    // asks which of them - if any - the message is meant for; only the picks
    // then receive the full trigger.
    //
    // Call on the world thread. `trigger` needs kind/chatType/roomKey/message
    // filled in; candidates must be eligible bots from the sender's group.
    // Returns false if the routing request could not be queued.
    bool RouteGroupMessage(Player* sender, std::vector<Player*> const& candidates, TriggerContext trigger);

    // Same idea for a real player's say/yell: instead of dice, one routing
    // request reads the message, the roster of bots in earshot, and the
    // conversation recently overheard around the sender, then picks whoever
    // the message is meant for - so an undirected "sure, how much?" right
    // after a bot's offer reaches that bot, and idle muttering reaches
    // nobody.
    //
    // Call on the world thread, after Overhear::RecordSpeech. `trigger`
    // needs kind/chatType/message filled in; candidates must be non-empty
    // (from BotSelector::CollectSayCandidates). Returns false if the routing
    // request could not be queued.
    bool RouteSayMessage(Player* sender, std::vector<Player*> const& candidates, TriggerContext trigger);

    // Extracts the first JSON array of strings from the router's reply.
    // nullopt when no parseable array is present (as opposed to a legitimate
    // empty [], which routes to nobody). Pure, unit-tested.
    std::optional<std::vector<std::string>> ParseRouterReply(std::string const& content);
}

#endif
