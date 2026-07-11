/*
 * This file is part of mod-llm for Felworld. Released under the MIT license
 * (see the LICENSE file at the module root).
 */

#ifndef MOD_LLM_TOOL_CALL_PARSER_H
#define MOD_LLM_TOOL_CALL_PARSER_H

#include <string>
#include <vector>

namespace ModLlm
{
    struct ToolCall
    {
        std::string name;
        std::string arguments; // raw JSON string, validated later by ToolRegistry
    };

    // The assistant message of one chat-completions response. content and
    // toolCalls can both be present; both empty is a valid "do nothing".
    struct LlmResponse
    {
        bool ok = false;
        std::string error;
        std::string content;
        std::vector<ToolCall> toolCalls;
    };

    namespace ToolCallParser
    {
        // Parses a non-streaming OpenAI chat-completions response body.
        // Pure function; safe on any thread.
        LlmResponse Parse(std::string const& body);
    }
}

#endif
