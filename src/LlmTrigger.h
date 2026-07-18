/*
 * This file is part of mod-llm for Felworld. Released under the MIT license
 * (see the LICENSE file at the module root).
 */

#ifndef MOD_LLM_TRIGGER_H
#define MOD_LLM_TRIGGER_H

#include "ObjectGuid.h"

#include <string>

namespace ModLlm
{
    // What woke the bot's brain up. Doubles as a bitmask so tools can declare
    // which triggers they are available for.
    enum TriggerKind : uint32
    {
        TRIGGER_CHAT_SAY     = 0x01, // /say or /yell in earshot
        TRIGGER_CHAT_WHISPER = 0x02, // whisper addressed to the bot
        TRIGGER_CHAT_PARTY   = 0x04, // party/raid chat
        TRIGGER_CHAT_GUILD   = 0x08, // guild/officer chat
        TRIGGER_CHAT_CHANNEL = 0x10, // named chat channel
        TRIGGER_EMOTE        = 0x20, // text emote performed nearby / at the bot
        TRIGGER_GAME_EVENT   = 0x40, // kill, death, level-up, quest, duel, ...
        TRIGGER_INITIATIVE   = 0x80, // idle tick: the bot may act unprompted

        TRIGGER_ALL          = 0xFF
    };

    constexpr char const* TriggerKindName(uint32 kind)
    {
        switch (kind)
        {
            case TRIGGER_CHAT_SAY:     return "say";
            case TRIGGER_CHAT_WHISPER: return "whisper";
            case TRIGGER_CHAT_PARTY:   return "party";
            case TRIGGER_CHAT_GUILD:   return "guild";
            case TRIGGER_CHAT_CHANNEL: return "channel";
            case TRIGGER_EMOTE:        return "emote";
            case TRIGGER_GAME_EVENT:   return "event";
            case TRIGGER_INITIATIVE:   return "initiative";
            default:                   return "unknown";
        }
    }

    // Snapshot of a trigger, safe to carry across threads: GUIDs and strings
    // only, never game object pointers.
    struct TriggerContext
    {
        uint32 kind = TRIGGER_INITIATIVE;
        ObjectGuid botGuid;
        ObjectGuid actorGuid;        // the triggering player, if any
        std::string actorName;
        uint32 chatType = 0;         // CHAT_MSG_* of the incoming message (reply routing)
        std::string channelName;     // for TRIGGER_CHAT_CHANNEL replies
        std::string roomKey;         // HistoryStore key of the shared room, empty if none
        std::string message;         // incoming chat text / emote description / event description
        std::string eventType;       // "creature_kill", "level_up", ... for TRIGGER_GAME_EVENT
        uint32 chainDepth = 0;       // bot-to-bot hops so far (caps runaway bot conversations)
        bool wantZoneChannel = false; // bind to the zone General channel at world-thread
                                      // dispatch (ChannelMgr is unsafe on map threads)
        bool crossFaction = false;   // the actor is on the opposing faction: no shared language
        bool crossFactionChatOk = false; // rolled permission to shout gibberish at them anyway
    };
}

#endif
