/*
 * This file is part of mod-llm for Felworld. Released under the MIT license
 * (see the LICENSE file at the module root).
 */

#include "TraceStore.h"

#include "DatabaseEnv.h"
#include "LlmConfig.h"
#include "LlmTrigger.h"

namespace ModLlm
{
    TraceStore* TraceStore::instance()
    {
        static TraceStore instance;
        return &instance;
    }

    void TraceStore::Load()
    {
        if (uint32 retentionDays = sLlmConfig->traceRetentionDays)
            CharacterDatabase.Execute(
                "DELETE FROM mod_llm_trace WHERE time < NOW() - INTERVAL {} DAY", retentionDays);
    }

    void TraceStore::Record(ObjectGuid botGuid, bool control, uint32 triggerKind, uint32 chainDepth,
        uint32 round, char const* status, uint32 latencyMs,
        std::string requestBody, std::string responseBody, std::string said)
    {
        // Control requests are currently all the group-chat router.
        PendingRow row{ botGuid.GetCounter(), control ? "router" : "main",
            TriggerKindName(triggerKind), chainDepth, round, status, latencyMs,
            std::move(requestBody), std::move(responseBody), std::move(said) };

        std::lock_guard<std::mutex> lock(_mutex);
        _pending.push_back(std::move(row));
    }

    void TraceStore::SaveDirty()
    {
        std::vector<PendingRow> rows;
        {
            std::lock_guard<std::mutex> lock(_mutex);
            rows.swap(_pending);
        }

        for (PendingRow& row : rows)
        {
            CharacterDatabase.EscapeString(row.request);
            CharacterDatabase.EscapeString(row.response);
            CharacterDatabase.EscapeString(row.said);
            CharacterDatabase.Execute(
                "INSERT INTO mod_llm_trace (bot_guid, kind, trigger_kind, chain_depth, round,"
                " status, latency_ms, request, response, said)"
                " VALUES ({}, '{}', '{}', {}, {}, '{}', {}, '{}', '{}', '{}')",
                row.botGuid, row.kind, row.triggerKind, row.chainDepth, row.round,
                row.status, row.latencyMs, row.request, row.response, row.said);
        }
    }
}
