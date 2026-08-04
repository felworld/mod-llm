/*
 * This file is part of mod-llm for Felworld. Released under the MIT license
 * (see the LICENSE file at the module root).
 */

#ifndef MOD_LLM_GUILD_FLAVOR_H
#define MOD_LLM_GUILD_FLAVOR_H

#include "Define.h"

#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

class Guild;

namespace ModLlm
{
    // The curated identities a bot-led guild can carry. Speech level only: a
    // flavor shapes how members talk about their guild, never what the
    // playerbots underneath actually go and do.
    enum class GuildFlavor : uint8
    {
        Leveling,
        Raiding,
        Pvp,
        Wpvp,
        Rp,
        Social
    };

    // Ordered on purpose: the first tag is the guild's lead identity, the rest
    // colour it.
    using FlavorProfile = std::vector<GuildFlavor>;

    struct WeightedProfile
    {
        FlavorProfile profile;
        uint32 weight = 0;
    };

    // Pure helpers - parsing, serialization, weighted picking, and every piece
    // of prompt copy composed out of a profile. No game state and no globals,
    // so they are unit-tested as plain functions.
    namespace GuildFlavors
    {
        char const* Name(GuildFlavor flavor);
        bool ParseTag(std::string const& text, GuildFlavor& out);
        bool Has(FlavorProfile const& profile, GuildFlavor flavor);

        // Canonical wire form, "rp+wpvp": what the DB column holds and what
        // the GM command accepts.
        std::string Serialize(FlavorProfile const& profile);
        bool Deserialize(std::string const& text, FlavorProfile& out);

        // "social+leveling:25, raiding:12" -> weighted list. An entry with an
        // unknown or repeated tag, or a weight that is not a positive number,
        // is logged and skipped rather than taking the whole option down.
        std::vector<WeightedProfile> ParseProfiles(std::string const& option);

        uint32 TotalWeight(std::vector<WeightedProfile> const& profiles);

        // Weighted pick; `roll` is taken modulo the total weight, so any
        // random source works. Null for an empty list.
        FlavorProfile const* Pick(std::vector<WeightedProfile> const& profiles, uint32 roll);

        // "a roleplay guild that also fights in world PvP" - the clause that
        // goes into every member's identity line, on every trigger.
        std::string IdentityClause(FlavorProfile const& profile);

        // Register guidance for guild chat, one sentence per tag present.
        std::string ChatGuidance(FlavorProfile const& profile);

        // Pitch guidance for the recruitment ad and the cold pitch. Grounded:
        // it sells what bots observably do and, for raiding, ambition rather
        // than a schedule nobody keeps.
        std::string RecruitGuidance(FlavorProfile const& profile);

        // Honesty guidance for the reactive side - somebody asking a bot with
        // invite rights about its guild.
        std::string InviteGuidance(FlavorProfile const& profile);

        // "flavor: ..." line appended to the grounded guild facts block.
        std::string FlavorLine(FlavorProfile const& profile);

        // Message of the day stamped on a freshly flavored guild that has
        // none of its own. `variant` picks between the primary tag's
        // templates; the templates themselves are fixed copy, never generated.
        std::string MotdFor(FlavorProfile const& profile, std::string const& guildName, uint32 variant);

        // Cold-pitch gate: a guild that does not level skips passersby too far
        // below the level cap to be worth pitching.
        bool WouldColdPitch(FlavorProfile const& profile, uint8 targetLevel, uint8 maxLevel);
    }

    // Guild id -> profile, persisted to characters.mod_llm_guild_flavor and
    // cached in memory. Reads happen on whichever thread builds a prompt, so
    // the cache is mutex-guarded; assignment, MOTD stamping, and the GM
    // overrides are world-thread only (they touch guilds and the DB).
    class GuildFlavorStore
    {
    public:
        static GuildFlavorStore* instance();

        void Load(); // synchronous, call once at startup

        // Cache read; false when the guild has no profile (player-founded
        // guilds never get one).
        bool Get(uint32 guildId, FlavorProfile& out);

        // World thread: flavor every bot-led guild with somebody online, and
        // stamp a message of the day on the ones still without one.
        void UpdateOnline();

        // World thread: roll and persist a profile if `guildId` is a bot-led
        // guild that has none yet.
        void EnsureAssigned(uint32 guildId);

        // GM overrides (world thread). Assign takes any profile, bot-led or
        // not; Reroll re-rolls from the configured weights and reports what it
        // landed on; Forget drops the profile, and the guild is flavored again
        // on the next pass if it is bot-led.
        void Assign(uint32 guildId, FlavorProfile const& profile);
        bool Reroll(uint32 guildId, FlavorProfile& out);
        void Forget(uint32 guildId);

        void Clear(); // tests only

    private:
        static bool Roll(FlavorProfile& out);
        static void StampMotd(Guild* guild, FlavorProfile const& profile);
        void Remember(uint32 guildId, FlavorProfile const& profile);
        void Persist(uint32 guildId, FlavorProfile const& profile);

        std::mutex _mutex;
        std::unordered_map<uint32, FlavorProfile> _profiles;
    };
}

#define sLlmGuildFlavors ModLlm::GuildFlavorStore::instance()

#endif
