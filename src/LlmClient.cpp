/*
 * This file is part of mod-llm for Felworld. Released under the MIT license
 * (see the LICENSE file at the module root).
 */

#include "LlmClient.h"

#include "Chat.h"
#include "LlmConfig.h"
#include "LlmToolOperation.h"
#include "Log.h"
#include "PlayerbotWorldThreadProcessor.h"
#include "PromptAssembler.h"
#include "SharedDefines.h"
#include "StringFormat.h"
#include "ToolCallParser.h"
#include "WorldPacket.h"
#include "WorldSessionMgr.h"

#include "httplib.h"

#include <nlohmann/json.hpp>
#include <string_view>

namespace ModLlm
{
    namespace
    {
        // How often the availability monitor probes the endpoint.
        constexpr uint32 PROBE_INTERVAL_SECONDS = 10;

        // Broadcasts a system-chat line to every online player. Queued from
        // the monitor thread, executed on the world thread.
        class WorldAnnounceOperation : public PlayerbotOperation
        {
        public:
            explicit WorldAnnounceOperation(std::string text) : _text(std::move(text)) { }

            bool Execute() override
            {
                WorldPacket data;
                ChatHandler::BuildChatPacket(data, CHAT_MSG_SYSTEM, LANG_UNIVERSAL, nullptr, nullptr, _text);
                sWorldSessionMgr->SendGlobalMessage(&data);
                return true;
            }

            std::string GetName() const override { return "LlmWorldAnnounce"; }

        private:
            std::string _text;
        };

        void AnnounceInGame(std::string text)
        {
            if (!sLlmConfig->announceEnabled)
                return;

            PlayerbotWorldThreadProcessor::instance().QueueOperation(
                std::make_unique<WorldAnnounceOperation>(std::move(text)));
        }

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

        _monitor = std::thread(&LlmClient::MonitorLoop, this);

        LOG_INFO("module.llm", "LlmClient started with {} worker(s), endpoint {}", workerCount, sLlmConfig->endpoint);
    }

    void LlmClient::Stop()
    {
        if (!_running.exchange(false))
            return;

        _wake.notify_all();
        _monitorWake.notify_all();
        for (std::thread& worker : _workers)
            if (worker.joinable())
                worker.join();
        _workers.clear();
        if (_monitor.joinable())
            _monitor.join();

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

    void LlmClient::MonitorLoop()
    {
        // Watches the endpoint so operators (console) and players (system
        // chat) learn when the LLM becomes usable - a fresh vLLM container
        // can spend minutes downloading and loading the model.
        bool everProbed = false;

        while (_running.load(std::memory_order_relaxed))
        {
            if (sLlmConfig->IsEnabled())
            {
                bool up = ProbeEndpoint();
                bool wasUp = _available.exchange(up, std::memory_order_relaxed);

                if (up && !wasUp)
                {
                    LOG_INFO("module.llm", "LLM endpoint {} is available; AI player chat is live",
                        sLlmConfig->endpoint);
                    AnnounceInGame("AI players are now online.");
                }
                else if (!up && wasUp)
                {
                    LOG_WARN("module.llm", "LLM endpoint {} became unreachable; AI player chat is paused",
                        sLlmConfig->endpoint);
                    AnnounceInGame("AI players are currently offline.");
                }
                else if (!up && !everProbed)
                    LOG_INFO("module.llm", "LLM endpoint {} is not reachable yet (model may still be "
                        "loading); probing every {}s", sLlmConfig->endpoint, PROBE_INTERVAL_SECONDS);

                everProbed = true;
            }

            std::unique_lock<std::mutex> lock(_monitorMutex);
            _monitorWake.wait_for(lock, std::chrono::seconds(PROBE_INTERVAL_SECONDS),
                [this] { return !_running.load(std::memory_order_relaxed); });
        }
    }

    bool LlmClient::ProbeEndpoint() const
    {
        std::string base;
        std::string path;
        if (!SplitEndpoint(sLlmConfig->endpoint, base, path))
            return false;

        // GET the models listing that lives next to the chat endpoint: every
        // OpenAI-compatible server implements it, and vLLM only answers once
        // the model has actually finished loading.
        std::string probePath = "/v1/models";
        constexpr std::string_view chatSuffix = "chat/completions";
        if (path.size() > chatSuffix.size() && path.ends_with(chatSuffix))
            probePath = path.substr(0, path.size() - chatSuffix.size()) + "models";

        httplib::Client client(base);
        client.set_connection_timeout(5, 0);
        client.set_read_timeout(5, 0);
        client.set_write_timeout(5, 0);
#ifdef CPPHTTPLIB_OPENSSL_SUPPORT
        client.enable_server_certificate_verification(false);
#endif

        httplib::Headers headers;
        if (!sLlmConfig->apiKey.empty())
            headers.emplace("Authorization", "Bearer " + sLlmConfig->apiKey);

        httplib::Result result = client.Get(probePath, headers);
        return result && result->status == 200;
    }

    void LlmClient::ProcessRequest(LlmRequest const& request)
    {
        nlohmann::json messages = PromptAssembler::BuildMessages(request.snapshot, request.trigger);
        for (nlohmann::json const& message : request.extraMessages)
            messages.push_back(message);

        nlohmann::json body = {
            { "model", sLlmConfig->model },
            { "messages", std::move(messages) },
            { "temperature", sLlmConfig->temperature },
            { "top_p", sLlmConfig->topP },
            { "max_tokens", sLlmConfig->maxTokens }
        };

        // vLLM extension (penalizes prompt tokens too, unlike frequency_penalty);
        // omitted at the neutral value so strict OpenAI-spec servers still work.
        if (sLlmConfig->repetitionPenalty != 1.0f)
            body["repetition_penalty"] = sLlmConfig->repetitionPenalty;

        if (!request.tools.empty())
        {
            body["tools"] = request.tools;
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
                std::move(response.content), request.round));
    }
}
