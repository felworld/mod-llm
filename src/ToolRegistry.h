/*
 * This file is part of mod-llm for Felworld. Released under the MIT license
 * (see the LICENSE file at the module root).
 */

#ifndef MOD_LLM_TOOL_REGISTRY_H
#define MOD_LLM_TOOL_REGISTRY_H

#include "Define.h"
#include "LlmTrigger.h"

#include <nlohmann/json.hpp>

#include <functional>
#include <string>
#include <vector>

class Player;
class PlayerbotAI;

namespace ModLlm
{
    // Everything a tool executor may touch. Only valid during the world-thread
    // execution of one LlmToolOperation; never store these pointers.
    struct ToolExecContext
    {
        Player* bot = nullptr;
        PlayerbotAI* ai = nullptr;
        Player* actor = nullptr; // nullptr when the trigger had no actor or they logged off
        TriggerContext const* trigger = nullptr;

        // Read tools write their answer here; a non-empty result is sent back
        // to the model as the tool message of a follow-up round. Cleared
        // before every executor call.
        std::string result;
    };

    // Returns true on success; on failure sets `error` (logged at debug level).
    // Tools that return data to the model write it to context.result.
    using ToolExecutor = std::function<bool(ToolExecContext&, nlohmann::json const& args, std::string& error)>;

    // Evaluated on the world thread when the tool list for a request is built,
    // so tools that cannot possibly succeed (invite while already grouped,
    // duel a dead player, ...) are never offered to the model. State can still
    // change before execution, so executors keep their own checks.
    using ToolAvailability = std::function<bool(Player* bot, Player* actor)>;

    // Finer-grained than triggerMask: offered only when the trigger itself
    // qualifies (e.g. the channel is a defense channel).
    using ToolTriggerFilter = std::function<bool(TriggerContext const&)>;

    struct ToolSpec
    {
        std::string name;
        std::string description;
        nlohmann::json parameters;  // JSON Schema object for the arguments
        uint32 triggerMask = TRIGGER_ALL;
        bool requiresActor = false;
        ToolExecutor execute;
        ToolAvailability available;  // optional; unset = always offered
        ToolTriggerFilter triggerFilter;  // optional; unset = any trigger matching the mask
    };

    // Adding a new bot capability = registering one ToolSpec (see LlmTools.cpp).
    // Registration happens once at startup, before the worker threads exist;
    // afterwards the registry is read-only, so lookups need no locking.
    class ToolRegistry
    {
    public:
        static ToolRegistry* instance();

        void Register(ToolSpec spec);
        void Clear(); // tests only

        ToolSpec const* Find(std::string const& name) const;

        // OpenAI "tools" array of every tool available for the given trigger
        // and game state. Call on the thread that owns bot/actor; actor may be
        // nullptr (tools that require one are then omitted). Pass the trigger
        // so tools with a triggerFilter can be offered selectively.
        nlohmann::json BuildToolsArray(uint32 triggerMask, Player* bot = nullptr, Player* actor = nullptr,
            TriggerContext const* trigger = nullptr) const;

        // Validates a parsed arguments object against a tool's schema:
        // object-ness, required keys, declared keys, primitive types, enums.
        static bool ValidateArgs(nlohmann::json const& schema, nlohmann::json const& args, std::string& error);

    private:
        std::vector<ToolSpec> _tools;
    };
}

#define sLlmToolRegistry ModLlm::ToolRegistry::instance()

#endif
