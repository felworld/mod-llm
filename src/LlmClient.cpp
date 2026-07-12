/*
 * This file is part of mod-llm for Felworld. Released under the MIT license
 * (see the LICENSE file at the module root).
 */

#include "LlmClient.h"

#include "LlmConfig.h"
#include "LlmToolOperation.h"
#include "Log.h"
#include "PlayerbotWorldThreadProcessor.h"
#include "PromptAssembler.h"
#include "StringFormat.h"
#include "ToolCallParser.h"
#include "ToolRegistry.h"

#include "httplib.h"

#include <nlohmann/json.hpp>

namespace ModLlm
{
    namespace
    {
        // Splits "http://host:port/path" into base ("http://host:port") and
        // path ("/path"). Returns false if the URL has no scheme.
        bool SplitEndpoint(std::string const& endpoint, std::string& base, std::string& path)
        {
            size_t schemeEnd = endpoint.find("://");
            if (schemeEnd == std::string::npos)
                return false;

            size_t pathStart = endpoint.find('/', schemeEnd + 3);
            if (pathStart == std::string::npos)
            {
                base = endpoint;
                path = "/v1/chat/completions";
            }
            else
            {
                base = endpoint.substr(0, pathStart);
                path = endpoint.substr(pathStart);
            }
            return true;
        }
    }

    LlmClient* LlmClient::instance()
    {
        static LlmClient instance;
        return &instance;
    }

    void LlmClient::Start()
    {
        if (_running.exchange(true))
            return;

        uint32 workerCount = std::max<uint32>(1, sLlmConfig->maxConcurrentRequests);
        for (uint32 i = 0; i < workerCount; ++i)
            _workers.emplace_back(&LlmClient::WorkerLoop, this);

        LOG_INFO("module.llm", "LlmClient started with {} worker(s), endpoint {}", workerCount, sLlmConfig->endpoint);
    }

    void LlmClient::Stop()
    {
        if (!_running.exchange(false))
            return;

        _wake.notify_all();
        for (std::thread& worker : _workers)
            if (worker.joinable())
                worker.join();
        _workers.clear();

        std::lock_guard<std::mutex> lock(_mutex);
        _queue.clear();
    }

    bool LlmClient::Submit(LlmRequest&& request)
    {
        if (!_running.load(std::memory_order_relaxed) || !sLlmConfig->IsEnabled())
            return false;

        {
            std::lock_guard<std::mutex> lock(_mutex);
            if (_queue.size() >= sLlmConfig->maxQueueSize)
            {
                LOG_WARN("module.llm", "Request queue full ({}), dropping request for bot {}",
                    _queue.size(), request.snapshot.botName);
                return false;
            }
            _queue.push_back(std::move(request));
        }

        _wake.notify_one();
        return true;
    }

    uint32 LlmClient::GetQueueSize()
    {
        std::lock_guard<std::mutex> lock(_mutex);
        return _queue.size();
    }

    void LlmClient::WorkerLoop()
    {
        while (true)
        {
            LlmRequest request;
            {
                std::unique_lock<std::mutex> lock(_mutex);
                _wake.wait(lock, [this] { return !_queue.empty() || !_running.load(std::memory_order_relaxed); });

                if (!_running.load(std::memory_order_relaxed))
                    return;

                request = std::move(_queue.front());
                _queue.pop_front();
            }

            try
            {
                ProcessRequest(request);
            }
            catch (std::exception const& e)
            {
                LOG_ERROR("module.llm", "Unhandled exception in LLM worker: {}", e.what());
                _failed.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }

    void LlmClient::ProcessRequest(LlmRequest const& request)
    {
        nlohmann::json body = {
            { "model", sLlmConfig->model },
            { "messages", PromptAssembler::BuildMessages(request.snapshot, request.trigger) },
            { "temperature", sLlmConfig->temperature },
            { "top_p", sLlmConfig->topP },
            { "max_tokens", sLlmConfig->maxTokens }
        };

        nlohmann::json tools = sLlmToolRegistry->BuildToolsArray(request.toolMask);
        if (!tools.empty())
        {
            body["tools"] = std::move(tools);
            body["tool_choice"] = "auto";
        }

        if (sLlmConfig->debugLogPrompts)
            LOG_INFO("module.llm", "Prompt for bot {}: {}", request.snapshot.botName, body.dump());

        std::string base;
        std::string path;
        if (!SplitEndpoint(sLlmConfig->endpoint, base, path))
        {
            LOG_ERROR("module.llm", "Invalid LLM.Endpoint '{}'", sLlmConfig->endpoint);
            _failed.fetch_add(1, std::memory_order_relaxed);
            return;
        }

        httplib::Client client(base);
        client.set_connection_timeout(sLlmConfig->timeoutSeconds, 0);
        client.set_read_timeout(sLlmConfig->timeoutSeconds, 0);
        client.set_write_timeout(sLlmConfig->timeoutSeconds, 0);
#ifdef CPPHTTPLIB_OPENSSL_SUPPORT
        client.enable_server_certificate_verification(false);
#endif

        httplib::Headers headers;
        if (!sLlmConfig->apiKey.empty())
            headers.emplace("Authorization", "Bearer " + sLlmConfig->apiKey);

        httplib::Result result = client.Post(path, headers, body.dump(), "application/json");
        if (!result || result->status != 200)
        {
            LOG_WARN("module.llm", "LLM request failed for bot {}: {}",
                request.snapshot.botName,
                result ? Acore::StringFormat("HTTP {}", result->status) : httplib::to_string(result.error()));
            _failed.fetch_add(1, std::memory_order_relaxed);
            return;
        }

        LlmResponse response = ToolCallParser::Parse(result->body);
        if (!response.ok)
        {
            LOG_WARN("module.llm", "Failed to parse LLM response for bot {}: {}",
                request.snapshot.botName, response.error);
            _failed.fetch_add(1, std::memory_order_relaxed);
            return;
        }

        _completed.fetch_add(1, std::memory_order_relaxed);

        if (sLlmConfig->debugEnabled)
        {
            std::string toolNames;
            for (ToolCall const& call : response.toolCalls)
            {
                if (!toolNames.empty())
                    toolNames += ", ";
                toolNames += call.name;
            }
            LOG_INFO("module.llm", "Bot {} response: tools [{}], content '{}'",
                request.snapshot.botName, toolNames, response.content);
        }

        if (response.toolCalls.empty() && response.content.empty())
            return; // the model chose to do nothing

        // Marshal all game-state effects onto the world thread.
        PlayerbotWorldThreadProcessor::instance().QueueOperation(
            std::make_unique<LlmToolOperation>(request.trigger, std::move(response.toolCalls),
                std::move(response.content)));
    }
}
