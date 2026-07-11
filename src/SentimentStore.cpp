/*
 * This file is part of mod-llm for Felworld. Released under the MIT license
 * (see the LICENSE file at the module root).
 */

#include "SentimentStore.h"

#include "DatabaseEnv.h"
#include "LlmConfig.h"
#include "Log.h"
#include "QueryResult.h"

#include <algorithm>

namespace ModLlm
{
    SentimentStore* SentimentStore::instance()
    {
        static SentimentStore instance;
        return &instance;
    }

    void SentimentStore::Load()
    {
        QueryResult result = CharacterDatabase.Query("SELECT bot_guid, player_guid, value FROM mod_llm_sentiment");
        if (!result)
            return;

        std::lock_guard<std::mutex> lock(_mutex);
        do
        {
            Field* fields = result->Fetch();
            uint64 key = (uint64(fields[0].Get<uint32>()) << 32) | fields[1].Get<uint32>();
            _values[key] = fields[2].Get<float>();
        } while (result->NextRow());

        LOG_INFO("module.llm", "Loaded {} sentiment entries", _values.size());
    }

    void SentimentStore::SaveDirty()
    {
        std::lock_guard<std::mutex> lock(_mutex);
        for (uint64 key : _dirty)
        {
            auto it = _values.find(key);
            if (it == _values.end())
                continue;

            CharacterDatabase.Execute(
                "REPLACE INTO mod_llm_sentiment (bot_guid, player_guid, value) VALUES ({}, {}, {})",
                uint32(key >> 32), uint32(key & 0xFFFFFFFF), it->second);
        }
        _dirty.clear();
    }

    float SentimentStore::Get(ObjectGuid botGuid, ObjectGuid playerGuid)
    {
        std::lock_guard<std::mutex> lock(_mutex);
        auto it = _values.find(MakeKey(botGuid, playerGuid));
        return it != _values.end() ? it->second : sLlmConfig->sentimentDefault;
    }

    void SentimentStore::Adjust(ObjectGuid botGuid, ObjectGuid playerGuid, float delta)
    {
        std::lock_guard<std::mutex> lock(_mutex);
        uint64 key = MakeKey(botGuid, playerGuid);
        auto it = _values.find(key);
        float value = it != _values.end() ? it->second : sLlmConfig->sentimentDefault;
        _values[key] = std::clamp(value + delta, 0.0f, 1.0f);
        _dirty.insert(key);
    }

    std::string SentimentStore::Describe(float value)
    {
        if (value < 0.2f)
            return "hostile";
        if (value < 0.4f)
            return "cold";
        if (value < 0.6f)
            return "neutral";
        if (value < 0.8f)
            return "warm";
        return "friendly";
    }
}
