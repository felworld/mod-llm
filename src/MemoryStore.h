/*
 * This file is part of mod-llm for Felworld. Released under the MIT license
 * (see the LICENSE file at the module root).
 */

#ifndef MOD_LLM_MEMORY_STORE_H
#define MOD_LLM_MEMORY_STORE_H

#include "ObjectGuid.h"

#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace ModLlm
{
    // Per-bot memory scratchpad: short notes the model writes via the
    // remember/forget tools and reads back through prompt injection. A note
    // is keyed by slug (upsert semantics, like a filename); subject_guid
    // scopes it to one player, 0 means a general note (goals, plans, world
    // facts). In-memory map persisted to characters.mod_llm_memory.
    // Mutex-guarded: reads happen from chat/map-thread hooks, writes from the
    // world thread.
    class MemoryStore
    {
    public:
        static MemoryStore* instance();

        void Load();      // synchronous, call once at startup
        void SaveDirty(); // async writes, call from the world-thread save tick

        // Empty return = stored; otherwise a model-facing error (memory full,
        // invalid slug). Overwriting an existing slug always succeeds.
        std::string Upsert(ObjectGuid botGuid, std::string const& slug, uint32 subjectGuid, std::string content);

        // False when the bot has no note under that slug.
        bool Forget(ObjectGuid botGuid, std::string const& slug);

        // Preformatted "- [slug] content" lines for prompt injection: notes
        // about subjectGuid first, then general notes, each newest first,
        // capped at maxLines. Notes about other players are never injected.
        std::string Format(ObjectGuid botGuid, uint32 subjectGuid, uint32 maxLines);

        // Lowercased, spaces/underscores to dashes, everything but [a-z0-9-]
        // stripped, truncated to fit the schema. May come back empty.
        static std::string NormalizeSlug(std::string const& slug);

        void Clear(); // tests only

    private:
        struct Note
        {
            std::string slug;
            uint32 subjectGuid = 0;
            std::string content;
            uint64 sequence = 0; // recency order; DB keeps the wall-clock time
        };

        struct PendingRow
        {
            uint32 botGuid = 0;
            std::string slug;
            uint32 subjectGuid = 0;
            std::string content;
            bool isDelete = false;
        };

        std::mutex _mutex;
        std::unordered_map<uint32, std::vector<Note>> _notes; // bot low-guid -> notes
        std::vector<PendingRow> _pending;
        uint64 _sequence = 0;
    };
}

#define sLlmMemoryStore ModLlm::MemoryStore::instance()

#endif
