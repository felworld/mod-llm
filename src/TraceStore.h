/*
 * This file is part of mod-llm for Felworld. Released under the MIT license
 * (see the LICENSE file at the module root).
 */

#ifndef MOD_LLM_TRACE_STORE_H
#define MOD_LLM_TRACE_STORE_H

#include "ObjectGuid.h"

#include <mutex>
#include <string>
#include <vector>

namespace ModLlm
{
    // Full-fidelity record of every LLM exchange (mod_llm_trace): the exact
    // request body sent, the raw response, and the say-text extracted from
    // it, keyed by bot and time. Always on - the point is that when a bot
    // says something odd in playtesting, the prompt that produced it is
    // already captured, searchable by phrase from the Grafana dashboards.
    // Record() runs on HTTP worker threads; rows flush to the characters DB
    // from the world thread (same pattern as HistoryStore).
    class TraceStore
    {
    public:
        static TraceStore* instance();

        // Purges rows older than LLM.Trace.RetentionDays. World thread, startup.
        void Load();

        void Record(ObjectGuid botGuid, bool control, uint32 triggerKind, uint32 chainDepth,
            uint32 round, char const* status, uint32 latencyMs,
            std::string requestBody, std::string responseBody, std::string said);

        void SaveDirty();

    private:
        struct PendingRow
        {
            uint32 botGuid;
            std::string kind;
            std::string triggerKind;
            uint32 chainDepth;
            uint32 round;
            std::string status;
            uint32 latencyMs;
            std::string request;
            std::string response;
            std::string said;
        };

        std::mutex _mutex;
        std::vector<PendingRow> _pending;
    };
}

#define sLlmTraceStore ModLlm::TraceStore::instance()

#endif
