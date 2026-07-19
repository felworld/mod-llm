/*
 * This file is part of mod-llm for Felworld. Released under the MIT license
 * (see the LICENSE file at the module root).
 */

#include "ToolRegistry.h"

#include "Log.h"

namespace ModLlm
{
    ToolRegistry* ToolRegistry::instance()
    {
        static ToolRegistry instance;
        return &instance;
    }

    void ToolRegistry::Register(ToolSpec spec)
    {
        LOG_DEBUG("module.llm", "Registering tool '{}'", spec.name);
        _tools.push_back(std::move(spec));
    }

    void ToolRegistry::Clear()
    {
        _tools.clear();
    }

    ToolSpec const* ToolRegistry::Find(std::string const& name) const
    {
        for (ToolSpec const& tool : _tools)
            if (tool.name == name)
                return &tool;

        return nullptr;
    }

    nlohmann::json ToolRegistry::BuildToolsArray(uint32 triggerMask, Player* bot, Player* actor,
        TriggerContext const* trigger) const
    {
        nlohmann::json tools = nlohmann::json::array();
        for (ToolSpec const& tool : _tools)
        {
            if (!(tool.triggerMask & triggerMask))
                continue;

            if (tool.requiresActor && !actor)
                continue;

            if (tool.triggerFilter && (!trigger || !tool.triggerFilter(*trigger)))
                continue;

            if (tool.available && !tool.available(bot, actor))
                continue;

            tools.push_back({
                { "type", "function" },
                { "function", {
                    { "name", tool.name },
                    { "description", tool.description },
                    { "parameters", tool.parameters }
                } }
            });
        }
        return tools;
    }

    bool ToolRegistry::ValidateArgs(nlohmann::json const& schema, nlohmann::json const& args, std::string& error)
    {
        if (!args.is_object())
        {
            error = "arguments must be a JSON object";
            return false;
        }

        nlohmann::json const properties = schema.value("properties", nlohmann::json::object());

        if (schema.contains("required"))
        {
            for (auto const& required : schema["required"])
            {
                if (!args.contains(required.get<std::string>()))
                {
                    error = "missing required argument '" + required.get<std::string>() + "'";
                    return false;
                }
            }
        }

        for (auto const& [key, value] : args.items())
        {
            if (!properties.contains(key))
            {
                error = "unknown argument '" + key + "'";
                return false;
            }

            nlohmann::json const& property = properties[key];
            std::string const type = property.value("type", "");

            if (type == "string" && !value.is_string())
            {
                error = "argument '" + key + "' must be a string";
                return false;
            }
            if (type == "boolean" && !value.is_boolean())
            {
                error = "argument '" + key + "' must be a boolean";
                return false;
            }
            if (type == "integer" && !value.is_number_integer())
            {
                error = "argument '" + key + "' must be an integer";
                return false;
            }
            if (type == "number" && !value.is_number())
            {
                error = "argument '" + key + "' must be a number";
                return false;
            }

            if (property.contains("enum"))
            {
                bool found = false;
                for (auto const& allowed : property["enum"])
                {
                    if (allowed == value)
                    {
                        found = true;
                        break;
                    }
                }
                if (!found)
                {
                    error = "argument '" + key + "' has a value outside its allowed set";
                    return false;
                }
            }
        }

        return true;
    }
}
