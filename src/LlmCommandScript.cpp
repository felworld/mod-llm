/*
 * This file is part of mod-llm for Felworld. Released under the MIT license
 * (see the LICENSE file at the module root).
 */

#include "Chat.h"
#include "CommandScript.h"
#include "LlmClient.h"
#include "LlmConfig.h"

using namespace Acore::ChatCommands;

namespace ModLlm
{
    class LlmCommandScript : public CommandScript
    {
    public:
        LlmCommandScript() : CommandScript("LlmCommandScript") { }

        ChatCommandTable GetCommands() const override
        {
            static ChatCommandTable llmCommandTable =
            {
                { "enable",  HandleEnable,  SEC_ADMINISTRATOR, Console::Yes },
                { "disable", HandleDisable, SEC_ADMINISTRATOR, Console::Yes },
                { "status",  HandleStatus,  SEC_ADMINISTRATOR, Console::Yes },
                { "reload",  HandleReload,  SEC_ADMINISTRATOR, Console::Yes },
            };

            static ChatCommandTable commandTable =
            {
                { "llm", llmCommandTable },
            };

            return commandTable;
        }

    private:
        static bool HandleEnable(ChatHandler* handler)
        {
            sLlmConfig->SetEnabled(true);
            sLlmClient->Start();
            handler->SendSysMessage("mod-llm enabled.");
            return true;
        }

        static bool HandleDisable(ChatHandler* handler)
        {
            sLlmConfig->SetEnabled(false);
            handler->SendSysMessage("mod-llm disabled. In-flight requests will be discarded.");
            return true;
        }

        static bool HandleStatus(ChatHandler* handler)
        {
            handler->PSendSysMessage("mod-llm: {} | endpoint: {} | model: {}",
                sLlmConfig->IsEnabled() ? "enabled" : "disabled", sLlmConfig->endpoint, sLlmConfig->model);
            handler->PSendSysMessage("workers: {} | queued: {} | completed: {} | failed: {}",
                sLlmClient->GetWorkerCount(), sLlmClient->GetQueueSize(),
                sLlmClient->GetCompletedCount(), sLlmClient->GetFailedCount());
            return true;
        }

        static bool HandleReload(ChatHandler* handler)
        {
            sLlmConfig->Load();
            handler->SendSysMessage("mod-llm configuration reloaded (worker count changes need a restart).");
            return true;
        }
    };
}

void AddSC_llm_command()
{
    new ModLlm::LlmCommandScript();
}
