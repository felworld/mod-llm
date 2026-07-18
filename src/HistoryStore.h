/*
 * This file is part of mod-llm for Felworld. Released under the MIT license
 * (see the LICENSE file at the module root).
 */

#ifndef MOD_LLM_HISTORY_STORE_H
#define MOD_LLM_HISTORY_STORE_H

#include "ObjectGuid.h"

#include <ctime>
#include <deque>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace ModLlm
{
    // Conversation transcripts, three kinds:
    //  - pair: one bot <-> one player (whispers, and any direct exchange)
    //  - room: a shared space (guild, group, named channel) keyed by string
    //  - overheard: per bot, every say/yell within its listen range - ambient
    //    short-term memory, in-memory only (one say near a crowd of bots
    //    would otherwise fan out into that many DB rows)
    // Ring buffers in memory, pair/room rows persisted to the characters DB
    // on the save tick. Mutex-guarded: appends come from chat hooks and the
    // world-thread tool executor.
    class HistoryStore
    {
    public:
        static HistoryStore* instance();

        void Load();      // synchronous, call once at startup
        void SaveDirty(); // async inserts, call from the world-thread save tick

        void AddPairLine(ObjectGuid botGuid, ObjectGuid playerGuid, bool botSpoke,
            std::string const& speakerName, std::string const& text);
        void AddRoomLine(std::string const& roomKey, ObjectGuid speakerGuid,
            std::string const& speakerName, std::string const& text);
        void AddOverheardLine(ObjectGuid botGuid, std::string const& speakerName,
            std::string const& text);

        // Transcripts preformatted with LLM.Prompt.HistoryLine, newest last,
        // one line per message. Empty string when there is no history.
        // Room and overheard lines older than LLM.History.ScrollbackSeconds
        // are omitted - like chat that has scrolled off a player's window -
        // while pair lines stay but pick up an age tag once they are stale.
        std::string FormatPair(ObjectGuid botGuid, ObjectGuid playerGuid, uint32 maxLines);
        std::string FormatRoom(std::string const& roomKey, uint32 maxLines);
        std::string FormatOverheard(ObjectGuid botGuid, uint32 maxLines);

    private:
        struct Line
        {
            time_t at;
            std::string speakerName;
            std::string text;
        };

        struct PendingPairRow
        {
            uint32 botGuid;
            uint32 playerGuid;
            bool botSpoke;
            std::string text;
        };

        struct PendingRoomRow
        {
            std::string roomKey;
            uint32 speakerGuid;
            std::string speakerName;
            std::string text;
        };

        static uint64 MakeKey(ObjectGuid botGuid, ObjectGuid playerGuid)
        {
            return (uint64(botGuid.GetCounter()) << 32) | playerGuid.GetCounter();
        }

        static std::string FormatLines(std::deque<Line> const& lines, uint32 maxLines, uint32 maxAgeSeconds);

        std::mutex _mutex;
        std::unordered_map<uint64, std::deque<Line>> _pairs;
        std::unordered_map<std::string, std::deque<Line>> _rooms;
        std::unordered_map<uint64, std::deque<Line>> _overheard;
        std::vector<PendingPairRow> _pendingPairRows;
        std::vector<PendingRoomRow> _pendingRoomRows;
    };
}

#define sLlmHistoryStore ModLlm::HistoryStore::instance()

#endif
