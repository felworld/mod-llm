/*
 * This file is part of mod-llm for Felworld. Released under the MIT license
 * (see the LICENSE file at the module root).
 */

#include "Chat.h"
#include "CommandScript.h"
#include "Guild.h"
#include "GuildFlavor.h"
#include "GuildMgr.h"
#include "LlmClient.h"
#include "LlmConfig.h"
#include "Player.h"

using namespace Acore::ChatCommands;

namespace ModLlm
{
    namespace
    {
        // Same identifier shape the core's `.guild info` takes: a guild id, or
        // a name in quotes when it has spaces.
        using GuildIdentifier = Variant<ObjectGuid::LowType, QuotedString>;

        Guild* FindGuild(GuildIdentifier const& identifier)
        {
            if (ObjectGuid::LowType const* guildId = std::get_if<ObjectGuid::LowType>(&identifier))
                return sGuildMgr->GetGuildById(*guildId);
            return sGuildMgr->GetGuildByName(identifier.get<QuotedString>());
        }
    }

    class LlmCommandScript : public CommandScript
    {
    public:
        LlmCommandScript() : CommandScript("LlmCommandScript") { }

        ChatCommandTable GetCommands() const override
        {
            static ChatCommandTable guildFlavorCommandTable =
            {
                { "",       HandleGuildFlavorShow,   SEC_ADMINISTRATOR, Console::Yes },
                { "set",    HandleGuildFlavorSet,    SEC_ADMINISTRATOR, Console::Yes },
                { "clear",  HandleGuildFlavorClear,  SEC_ADMINISTRATOR, Console::Yes },
                { "reroll", HandleGuildFlavorReroll, SEC_ADMINISTRATOR, Console::Yes },
            };

            static ChatCommandTable llmCommandTable =
            {
                { "enable",      HandleEnable,  SEC_ADMINISTRATOR, Console::Yes },
                { "disable",     HandleDisable, SEC_ADMINISTRATOR, Console::Yes },
                { "status",      HandleStatus,  SEC_ADMINISTRATOR, Console::Yes },
                { "reload",      HandleReload,  SEC_ADMINISTRATOR, Console::Yes },
                { "guildflavor", guildFlavorCommandTable },
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
            handler->PSendSysMessage("mod-llm: {} | endpoint: {} ({}) | model: {}",
                sLlmConfig->IsEnabled() ? "enabled" : "disabled", sLlmConfig->endpoint,
                sLlmClient->IsAvailable() ? "reachable" : "unreachable", sLlmConfig->model);
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

        // .llm guildflavor ["Guild Name"|id] - with no argument, the selected
        // player's (or your own) guild.
        static bool HandleGuildFlavorShow(ChatHandler* handler, Optional<GuildIdentifier> const& identifier)
        {
            Guild* guild = nullptr;
            if (identifier)
                guild = FindGuild(*identifier);
            else if (Optional<PlayerIdentifier> target = PlayerIdentifier::FromTargetOrSelf(handler);
                target && target->IsConnected())
                guild = target->GetConnectedPlayer()->GetGuild();

            if (!guild)
            {
                handler->SendErrorMessage("No such guild. Name it (in quotes if it has spaces) or its id, "
                    "or select one of its members.");
                return false;
            }

            FlavorProfile profile;
            if (!sLlmGuildFlavors->Get(guild->GetId(), profile))
            {
                handler->PSendSysMessage("<{}> ({}) has no flavor profile - player-founded guilds never get one.",
                    guild->GetName(), guild->GetId());
                return true;
            }

            handler->PSendSysMessage("<{}> ({}): {} - {}", guild->GetName(), guild->GetId(),
                GuildFlavors::Serialize(profile), GuildFlavors::IdentityClause(profile));
            return true;
        }

        // .llm guildflavor set "Guild Name" rp+wpvp
        static bool HandleGuildFlavorSet(ChatHandler* handler, GuildIdentifier const& identifier,
            std::string const& tags)
        {
            Guild* guild = FindGuild(identifier);
            if (!guild)
            {
                handler->SendErrorMessage("No such guild.");
                return false;
            }

            FlavorProfile profile;
            if (!GuildFlavors::Deserialize(tags, profile))
            {
                handler->SendErrorMessage("Unknown or repeated tag in '{}'. Tags are leveling, raiding, pvp, "
                    "wpvp, rp, social - joined with +, lead identity first.", tags);
                return false;
            }

            sLlmGuildFlavors->Assign(guild->GetId(), profile);
            handler->PSendSysMessage("<{}> ({}) is now {} - {}", guild->GetName(), guild->GetId(),
                GuildFlavors::Serialize(profile), GuildFlavors::IdentityClause(profile));
            return true;
        }

        // .llm guildflavor clear "Guild Name" - a bot-led guild picks a new
        // profile up on the next pass; the message of the day already stamped
        // stays as it is.
        static bool HandleGuildFlavorClear(ChatHandler* handler, GuildIdentifier const& identifier)
        {
            Guild* guild = FindGuild(identifier);
            if (!guild)
            {
                handler->SendErrorMessage("No such guild.");
                return false;
            }

            sLlmGuildFlavors->Forget(guild->GetId());
            handler->PSendSysMessage("<{}> ({}) has no flavor profile any more.",
                guild->GetName(), guild->GetId());
            return true;
        }

        // .llm guildflavor reroll "Guild Name"
        static bool HandleGuildFlavorReroll(ChatHandler* handler, GuildIdentifier const& identifier)
        {
            Guild* guild = FindGuild(identifier);
            if (!guild)
            {
                handler->SendErrorMessage("No such guild.");
                return false;
            }

            FlavorProfile profile;
            if (!sLlmGuildFlavors->Reroll(guild->GetId(), profile))
            {
                handler->SendErrorMessage("No usable LLM.GuildFlavor.Profiles entries to roll from.");
                return false;
            }

            handler->PSendSysMessage("<{}> ({}) rerolled to {} - {}", guild->GetName(), guild->GetId(),
                GuildFlavors::Serialize(profile), GuildFlavors::IdentityClause(profile));
            return true;
        }
    };
}

void AddSC_llm_command()
{
    new ModLlm::LlmCommandScript();
}
