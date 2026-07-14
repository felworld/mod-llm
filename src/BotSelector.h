/*
 * This file is part of mod-llm for Felworld. Released under the MIT license
 * (see the LICENSE file at the module root).
 */

#ifndef MOD_LLM_BOT_SELECTOR_H
#define MOD_LLM_BOT_SELECTOR_H

#include "Define.h"

#include <string>
#include <vector>

class Channel;
class Group;
class Guild;
class Player;

namespace ModLlm::BotSelector
{
    // True for a human-controlled player (no bot AI attached).
    bool IsRealPlayer(Player* player);

    // True if any human player is within `distance` of `bot` on its map.
    bool HasRealPlayerNearby(Player* bot, float distance);

    // Case-insensitive whole-word search; pure, unit-tested.
    bool MentionsName(std::string const& message, std::string const& name);

    // Per-channel-kind reply chance (percent), split by sender type.
    uint32 ReplyChance(uint32 triggerKind, bool senderIsBot);

    // Every eligible bot in the group that could react to `sender`'s message
    // (bot AI attached, not the sender, in-combat skip, human audience). No
    // chance roll or cap - callers decide how to narrow the list.
    std::vector<Player*> CollectGroupBots(Player* sender, Group* group);

    // Picks the bots that should react to a chat message. Handles eligibility
    // (audience of the message + a real player involved), chance rolls, the
    // name-mention override, the in-combat skip, and the MaxBotsToPick cap.
    // Call on the world thread (chat hooks).
    std::vector<Player*> SelectForChat(Player* sender, uint32 triggerKind, std::string const& message,
        Group* group, Guild* guild, Channel* channel, float maxDistance);

    // Picks nearby bot commentators for a game event or emote around `source`.
    // Same-map only (event hooks can run on map threads). No chance roll -
    // the caller owns per-event chances and cooldowns.
    std::vector<Player*> SelectNearby(Player* source, float distance, uint32 maxBots, bool includeSource);
}

#endif
