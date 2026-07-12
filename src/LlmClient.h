/*
 * This file is part of mod-llm for Felworld. Released under the MIT license
 * (see the LICENSE file at the module root).
 */

#ifndef MOD_LLM_CLIENT_H
#define MOD_LLM_CLIENT_H

#include "ContextBuilder.h"
#include "LlmTrigger.h"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

namespace ModLlm
{
    struct LlmRequest
    {
        ContextSnapshot snapshot;
        TriggerContext trigger;
        uint32 toolMask = TRIGGER_ALL;
    };

    // Bounded pool of HTTP worker threads. Submit() never blocks the caller;
    // workers POST to the OpenAI-compatible endpoint, parse the response, and
    // queue an LlmToolOperation for the world thread. No game state is touched
    // off the world thread.
    class LlmClient
    {
    public:
        static LlmClient* instance();

        void Start();
        void Stop();

        // False if the module is disabled, the pool is not running, or the
        // queue is full (the request is dropped - a stalled LLM server must
        // never back up into the world).
        bool Submit(LlmRequest&& request);

        uint32 GetQueueSize();
        uint32 GetWorkerCount() const { return uint32(_workers.size()); }
        uint64 GetCompletedCount() const { return _completed.load(std::memory_order_relaxed); }
        uint64 GetFailedCount() const { return _failed.load(std::memory_order_relaxed); }

        // Last result of the periodic endpoint probe. False until the first
        // successful probe (e.g. while vLLM is still loading the model).
        bool IsAvailable() const { return _available.load(std::memory_order_relaxed); }

    private:
        void WorkerLoop();
        void ProcessRequest(LlmRequest const& request);
        void MonitorLoop();
        bool ProbeEndpoint() const;

        std::mutex _mutex;
        std::condition_variable _wake;
        std::deque<LlmRequest> _queue;
        std::vector<std::thread> _workers;
        std::atomic<bool> _running{false};
        std::atomic<uint64> _completed{0};
        std::atomic<uint64> _failed{0};

        std::thread _monitor;
        std::mutex _monitorMutex;
        std::condition_variable _monitorWake;
        std::atomic<bool> _available{false};
    };
}

#define sLlmClient ModLlm::LlmClient::instance()

#endif
