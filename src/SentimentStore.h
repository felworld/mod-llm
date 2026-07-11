/*
 * This file is part of mod-llm for Felworld. Released under the MIT license
 * (see the LICENSE file at the module root).
 */

#ifndef MOD_LLM_SENTIMENT_STORE_H
#define MOD_LLM_SENTIMENT_STORE_H

#include "ObjectGuid.h"

#include <mutex>
#include <unordered_map>
#include <unordered_set>

namespace ModLlm
{
    // Bot -> player disposition, 0.0 (hostile) .. 0.5 (neutral) .. 1.0 (friendly).
    // In-memory map persisted to characters.mod_llm_sentiment. Mutex-guarded:
    // reads happen from chat/map-thread hooks, writes from the world thread.
    class SentimentStore
    {
    public:
        static SentimentStore* instance();

        void Load();      // synchronous, call once at startup
        void SaveDirty(); // async inserts, call from the world-thread save tick

        float Get(ObjectGuid botGuid, ObjectGuid playerGuid);
        void Adjust(ObjectGuid botGuid, ObjectGuid playerGuid, float delta);

        // "hostile" / "cold" / "neutral" / "warm" / "friendly"
        static std::string Describe(float value);

    private:
        static uint64 MakeKey(ObjectGuid botGuid, ObjectGuid playerGuid)
        {
            return (uint64(botGuid.GetCounter()) << 32) | playerGuid.GetCounter();
        }

        std::mutex _mutex;
        std::unordered_map<uint64, float> _values;
        std::unordered_set<uint64> _dirty;
    };
}

#define sLlmSentimentStore ModLlm::SentimentStore::instance()

#endif
