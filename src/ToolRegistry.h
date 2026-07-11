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
    };

    // Returns true on success; on failure sets `error` (logged at debug level).
    using ToolExecutor = std::function<bool(ToolExecContext&, nlohmann::json const& args, std::string& error)>;

    struct ToolSpec
    {
        std::string name;
        std::string description;
        nlohmann::json parameters;  // JSON Schema object for the arguments
        uint32 triggerMask = TRIGGER_ALL;
        bool requiresActor = false;
        ToolExecutor execute;
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

        // OpenAI "tools" array of every tool available for the given trigger.
        nlohmann::json BuildToolsArray(uint32 triggerMask) const;

        // Validates a parsed arguments object against a tool's schema:
        // object-ness, required keys, declared keys, primitive types, enums.
        static bool ValidateArgs(nlohmann::json const& schema, nlohmann::json const& args, std::string& error);

    private:
        std::vector<ToolSpec> _tools;
    };
}

#define sLlmToolRegistry ModLlm::ToolRegistry::instance()

#endif
