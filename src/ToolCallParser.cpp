/*
 * This file is part of mod-llm for Felworld. Released under the MIT license
 * (see the LICENSE file at the module root).
 */

#include "ToolCallParser.h"

#include <nlohmann/json.hpp>

namespace ModLlm::ToolCallParser
{
    LlmResponse Parse(std::string const& body)
    {
        LlmResponse response;

        nlohmann::json root = nlohmann::json::parse(body, nullptr, false);
        if (root.is_discarded())
        {
            response.error = "response body is not valid JSON";
            return response;
        }

        if (root.contains("error"))
        {
            auto const& error = root["error"];
            response.error = error.is_object() && error.contains("message") && error["message"].is_string()
                ? error["message"].get<std::string>()
                : error.dump();
            return response;
        }

        if (!root.contains("choices") || !root["choices"].is_array() || root["choices"].empty())
        {
            response.error = "response has no choices";
            return response;
        }

        auto const& choice = root["choices"][0];
        if (!choice.contains("message") || !choice["message"].is_object())
        {
            response.error = "choice has no message";
            return response;
        }

        auto const& message = choice["message"];
        if (message.contains("content") && message["content"].is_string())
            response.content = message["content"].get<std::string>();

        if (message.contains("tool_calls") && message["tool_calls"].is_array())
        {
            for (auto const& call : message["tool_calls"])
            {
                if (!call.is_object() || !call.contains("function") || !call["function"].is_object())
                    continue;

                auto const& function = call["function"];
                if (!function.contains("name") || !function["name"].is_string())
                    continue;

                ToolCall toolCall;
                toolCall.name = function["name"].get<std::string>();

                // Servers may omit ids; fabricate stable ones so error
                // feedback can always reference the call.
                if (call.contains("id") && call["id"].is_string())
                    toolCall.id = call["id"].get<std::string>();
                else
                    toolCall.id = "call_" + std::to_string(response.toolCalls.size());

                if (function.contains("arguments"))
                {
                    // Per the OpenAI schema, arguments is a JSON-encoded
                    // string; tolerate servers that inline an object.
                    if (function["arguments"].is_string())
                        toolCall.arguments = function["arguments"].get<std::string>();
                    else if (function["arguments"].is_object())
                        toolCall.arguments = function["arguments"].dump();
                }

                if (toolCall.arguments.empty())
                    toolCall.arguments = "{}";

                response.toolCalls.push_back(std::move(toolCall));
            }
        }

        response.ok = true;
        return response;
    }
}
