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

namespace ModLlm
{
    struct TriggerContext;
}

namespace ModLlm::BotSelector
{
    // True for a human-controlled player (no bot AI attached).
    bool IsRealPlayer(Player* player);

    // True if any human player is within `distance` of `bot` on its map.
    bool HasRealPlayerNearby(Player* bot, float distance);

    // True if any human player is currently on the channel. World thread only.
    bool HasRealPlayerInChannel(Channel* channel);

    // True if any human player is a member of the group. Reads member slots +
    // ObjectAccessor only, so it is safe from map-update threads.
    bool GroupHasRealPlayer(Group* group);

    // Whether `bot` shares a chat language with `speaker`: same faction, or
    // the server allows cross-faction chat.
    bool CanUnderstand(Player* bot, Player* speaker);

    // Case-insensitive whole-word search; pure, unit-tested.
    bool MentionsName(std::string const& message, std::string const& name);

    // Per-channel-kind reply chance (percent), split by sender type.
    uint32 ReplyChance(uint32 triggerKind, bool senderIsBot);

    // Every eligible bot in the group that could react to `sender`'s message
    // (bot AI attached, not the sender, in-combat skip, human audience). No
    // chance roll or cap - callers decide how to narrow the list.
    std::vector<Player*> CollectGroupBots(Player* sender, Group* group);

    // Every eligible bot in earshot of `sender`'s say/yell (same map, can
    // understand the sender's faction, human audience). No chance roll or
    // cap - callers decide how to narrow the list.
    std::vector<Player*> CollectSayCandidates(Player* sender, float maxDistance);

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

    // Every bot within `distance` of `speaker` (same map) that can understand
    // it. Hearing is passive: no combat skip, no audience requirement.
    std::vector<Player*> CollectListeners(Player* speaker, float distance);

    // Points `trigger`'s reply at the bot's current zone General channel
    // (chatType, channelName, roomKey) when the bot is on it and a real
    // player is there to read it; returns false (trigger untouched)
    // otherwise. World thread only (ChannelMgr access).
    bool BindZoneChannel(Player* bot, TriggerContext& trigger);
}

#endif
