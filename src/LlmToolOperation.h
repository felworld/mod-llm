/*
 * This file is part of mod-llm for Felworld. Released under the MIT license
 * (see the LICENSE file at the module root).
 */

#ifndef MOD_LLM_TOOL_OPERATION_H
#define MOD_LLM_TOOL_OPERATION_H

#include "LlmTrigger.h"
#include "PlayerbotOperation.h"
#include "ToolCallParser.h"

#include <string>
#include <utility>
#include <vector>

class Player;

namespace ModLlm
{
    // Executes the tool calls of one LLM response on the world thread (via
    // PlayerbotWorldThreadProcessor). Carries GUIDs and strings only; players
    // are re-resolved at execution time and the operation degrades gracefully
    // if they are gone.
    class LlmToolOperation : public PlayerbotOperation
    {
    public:
        LlmToolOperation(TriggerContext trigger, std::vector<ToolCall> toolCalls, std::string bareContent,
            uint32 round = 0)
            : _trigger(std::move(trigger)), _toolCalls(std::move(toolCalls)), _bareContent(std::move(bareContent))
            , _round(round)
        {
        }

        bool Execute() override;
        bool IsValid() const override;
        ObjectGuid GetBotGuid() const override { return _trigger.botGuid; }
        uint32 GetPriority() const override { return 50; } // player-facing
        std::string GetName() const override { return "LlmToolOperation"; }

    private:
        void SubmitErrorFeedback(Player* bot, Player* actor,
            std::vector<std::pair<bool, std::string>> const& outcomes) const;

        TriggerContext _trigger;
        std::vector<ToolCall> _toolCalls;
        std::string _bareContent;
        uint32 _round = 0;
    };
}

#endif
