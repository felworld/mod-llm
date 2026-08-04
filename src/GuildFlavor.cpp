/*
 * This file is part of mod-llm for Felworld. Released under the MIT license
 * (see the LICENSE file at the module root).
 */

#include "GuildFlavor.h"

#include "CharacterCache.h"
#include "DatabaseEnv.h"
#include "Guild.h"
#include "GuildMgr.h"
#include "LlmConfig.h"
#include "Log.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "PlayerbotAIConfig.h"
#include "QueryResult.h"
#include "Random.h"
#include "RandomPlayerbotMgr.h"
#include "StringFormat.h"
#include "WorldSession.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <iterator>

namespace ModLlm
{
    namespace
    {
        // Guild MOTDs are read by humans as well as bots, so the copy is
        // written out here rather than generated. One row per tag, indexed by
        // the enum: the noun phrase leads an identity clause, the verb phrase
        // follows one, and the guidance sentences are what the model is told
        // about the register to write in.
        struct FlavorCopy
        {
            char const* name;
            char const* identityNoun;
            char const* identityVerb;
            char const* chatGuidance;
            char const* recruitGuidance;
            char const* motdClause;
            char const* motdPrimary[2];
        };

        constexpr FlavorCopy COPY[] =
        {
            // Leveling
            {
                "leveling",
                "a leveling guild",
                "levels together",
                " Guild chat there runs on levels, quests, and dungeon runs - who is where and who needs a hand.",
                " What you offer: people to level with and a hand when a quest turns rough, at any level.",
                " Leveling runs get called in guild chat.",
                {
                    "Leveling together - shout in guild chat when you need a hand.",
                    "{guild}: nobody levels alone here. Ask, and someone comes."
                }
            },
            // Raiding
            {
                "raiding",
                "a raiding guild",
                "is working its way toward raids",
                " Guild chat there trends toward progression - gear, specs, lockouts, and what the group is"
                    " working on next.",
                " What you offer: ambition - the guild is building toward raid content and wants people who"
                    " want the same.",
                " Raid ambitions welcome.",
                {
                    "Gearing up and pushing forward. Bring your best spec.",
                    "{guild} is building a raid team - progression over standing in town."
                }
            },
            // Pvp
            {
                "pvp",
                "a PvP guild",
                "runs battlegrounds together",
                " Guild chat there trends toward battlegrounds - queues, honor, premades, and how the last"
                    " match went.",
                " What you offer: a group that queues battlegrounds together instead of going in solo.",
                " Battleground groups form in guild chat.",
                {
                    "Queues pop in guild chat. Bring honor gear or come earn it.",
                    "{guild} runs battlegrounds together - ask for a group, never queue alone."
                }
            },
            // Wpvp
            {
                "wpvp",
                "a world PvP guild",
                "fights in world PvP",
                " Guild chat there trends toward the open world - enemy sightings, ganks, and who is riding"
                    " out to answer them.",
                " What you offer: the open world - contested zones, sightings called out, and people who ride"
                    " out to answer them.",
                " World PvP calls get answered.",
                {
                    "Call out sightings in guild chat and we ride.",
                    "{guild} holds the contested zones. Say where, we come."
                }
            },
            // Rp
            {
                "rp",
                "a roleplay guild",
                "keeps to character",
                " Guild chat there is in character: speak as your character speaks, in their own voice and"
                    " their own words.",
                " What you offer: a guild played in character, where guild chat is part of the story.",
                " Guild chat stays in character.",
                {
                    "In character in guild chat. Welcome to {guild}.",
                    "{guild}: our story is told in character - speak as your character speaks."
                }
            },
            // Social
            {
                "social",
                "a casual social guild",
                "is here for the company",
                " Guild chat there is easy company - small talk, jokes, and whatever the day turned up.",
                " What you offer: easy company - people to talk to and run things with, no pressure attached.",
                " Company counts as much as content.",
                {
                    "Good company first. Say hello, stay a while.",
                    "{guild} is here for the people - talk, group up, no pressure."
                }
            }
        };

        static_assert(std::size(COPY) == size_t(GuildFlavor::Social) + 1, "flavor copy table out of sync");

        // A couple of classic pairings read stiff when composed clause by
        // clause, so they get their own message of the day.
        struct BespokeMotd
        {
            GuildFlavor first;
            GuildFlavor second;
            char const* templates[2];
        };

        constexpr BespokeMotd BESPOKE_MOTD[] =
        {
            {
                GuildFlavor::Rp, GuildFlavor::Wpvp,
                {
                    "{guild}: in character, and in the fight - the contested lands are where our story happens.",
                    "The banners ride at dusk. {guild} keeps to character and keeps to the field."
                }
            },
            {
                GuildFlavor::Raiding, GuildFlavor::Pvp,
                {
                    "{guild}: progression and battlegrounds both - same appetite either way.",
                    "{guild} pushes content and pops queues. Bring gear, or come and get it."
                }
            }
        };

        // The client truncates a longer message of the day anyway.
        constexpr size_t MAX_MOTD_LENGTH = 128;

        FlavorCopy const& Copy(GuildFlavor flavor)
        {
            return COPY[size_t(flavor)];
        }

        std::string Trim(std::string const& text)
        {
            size_t begin = 0;
            size_t end = text.size();
            while (begin < end && std::isspace(static_cast<unsigned char>(text[begin])))
                ++begin;
            while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1])))
                --end;
            return text.substr(begin, end - begin);
        }

        std::string Substitute(std::string text, std::string const& guildName)
        {
            std::string const placeholder = "{guild}";
            size_t pos = 0;
            while ((pos = text.find(placeholder, pos)) != std::string::npos)
            {
                text.replace(pos, placeholder.size(), guildName);
                pos += guildName.size();
            }
            return text;
        }

        // The guild speaks for whoever built it: a guild founded and led by a
        // random bot gets a flavor, a player-founded one never does.
        bool IsBotLed(Guild const* guild)
        {
            ObjectGuid leaderGuid = guild->GetLeaderGUID();
            if (Player* leader = ObjectAccessor::FindPlayer(leaderGuid))
                return sRandomPlayerbotMgr.IsRandomBot(leader);

            // The leader is offline, and IsRandomBot only knows the bots that
            // are spawned right now - fall back to the account it lives on,
            // which is what that check reads underneath.
            return sPlayerbotAIConfig.IsInRandomAccountList(
                sCharacterCache->GetCharacterAccountIdByGuid(leaderGuid));
        }
    }

    namespace GuildFlavors
    {
        char const* Name(GuildFlavor flavor)
        {
            return Copy(flavor).name;
        }

        bool ParseTag(std::string const& text, GuildFlavor& out)
        {
            std::string lowered;
            for (char c : Trim(text))
                lowered += char(std::tolower(static_cast<unsigned char>(c)));

            for (size_t i = 0; i < std::size(COPY); ++i)
            {
                if (lowered == COPY[i].name)
                {
                    out = GuildFlavor(i);
                    return true;
                }
            }
            return false;
        }

        bool Has(FlavorProfile const& profile, GuildFlavor flavor)
        {
            return std::find(profile.begin(), profile.end(), flavor) != profile.end();
        }

        std::string Serialize(FlavorProfile const& profile)
        {
            std::string text;
            for (GuildFlavor flavor : profile)
            {
                if (!text.empty())
                    text += '+';
                text += Name(flavor);
            }
            return text;
        }

        bool Deserialize(std::string const& text, FlavorProfile& out)
        {
            FlavorProfile parsed;
            size_t begin = 0;
            while (begin <= text.size())
            {
                size_t plus = text.find('+', begin);
                std::string token = Trim(text.substr(begin,
                    plus == std::string::npos ? std::string::npos : plus - begin));

                GuildFlavor flavor = GuildFlavor::Leveling;
                if (token.empty() || !ParseTag(token, flavor) || Has(parsed, flavor))
                    return false;

                parsed.push_back(flavor);
                if (plus == std::string::npos)
                    break;
                begin = plus + 1;
            }

            if (parsed.empty())
                return false;

            out = std::move(parsed);
            return true;
        }

        std::vector<WeightedProfile> ParseProfiles(std::string const& option)
        {
            std::vector<WeightedProfile> profiles;

            size_t begin = 0;
            while (begin <= option.size())
            {
                size_t comma = option.find(',', begin);
                std::string entry = Trim(option.substr(begin,
                    comma == std::string::npos ? std::string::npos : comma - begin));
                if (comma == std::string::npos)
                    begin = option.size() + 1;
                else
                    begin = comma + 1;

                if (entry.empty())
                    continue;

                size_t colon = entry.find(':');
                if (colon == std::string::npos)
                {
                    LOG_ERROR("module.llm", "Guild flavor profile '{}' has no ':<weight>' - skipped", entry);
                    continue;
                }

                FlavorProfile profile;
                if (!Deserialize(entry.substr(0, colon), profile))
                {
                    LOG_ERROR("module.llm", "Guild flavor profile '{}' has an unknown or repeated tag - skipped",
                        entry);
                    continue;
                }

                std::string weightText = Trim(entry.substr(colon + 1));
                uint32 weight = 0;
                bool digits = !weightText.empty();
                for (char c : weightText)
                    if (!std::isdigit(static_cast<unsigned char>(c)))
                        digits = false;
                if (digits)
                    weight = uint32(std::strtoul(weightText.c_str(), nullptr, 10));

                if (!weight)
                {
                    LOG_ERROR("module.llm", "Guild flavor profile '{}' has no positive weight - skipped", entry);
                    continue;
                }

                profiles.push_back({ std::move(profile), weight });
            }

            return profiles;
        }

        uint32 TotalWeight(std::vector<WeightedProfile> const& profiles)
        {
            uint32 total = 0;
            for (WeightedProfile const& profile : profiles)
                total += profile.weight;
            return total;
        }

        FlavorProfile const* Pick(std::vector<WeightedProfile> const& profiles, uint32 roll)
        {
            uint32 total = TotalWeight(profiles);
            if (!total)
                return nullptr;

            uint32 target = roll % total;
            for (WeightedProfile const& profile : profiles)
            {
                if (target < profile.weight)
                    return &profile.profile;
                target -= profile.weight;
            }
            return &profiles.back().profile;
        }

        std::string IdentityClause(FlavorProfile const& profile)
        {
            if (profile.empty())
                return "";

            std::string clause = Copy(profile.front()).identityNoun;
            for (size_t i = 1; i < profile.size(); ++i)
            {
                clause += i == 1 ? " that also " : " and ";
                clause += Copy(profile[i]).identityVerb;
            }
            return clause;
        }

        std::string ChatGuidance(FlavorProfile const& profile)
        {
            std::string guidance;
            for (GuildFlavor flavor : profile)
                guidance += Copy(flavor).chatGuidance;
            return guidance;
        }

        std::string RecruitGuidance(FlavorProfile const& profile)
        {
            std::string guidance;
            for (GuildFlavor flavor : profile)
                guidance += Copy(flavor).recruitGuidance;
            return guidance;
        }

        std::string InviteGuidance(FlavorProfile const& profile)
        {
            if (profile.empty())
                return "";

            return Acore::StringFormat(" Your guild is {} - describe it that way to anyone who asks, and use"
                " the guild_invite tool when you want them in. Anyone may ask, whoever they are.",
                IdentityClause(profile));
        }

        std::string FlavorLine(FlavorProfile const& profile)
        {
            if (profile.empty())
                return "";
            return "flavor: " + IdentityClause(profile);
        }

        std::string MotdFor(FlavorProfile const& profile, std::string const& guildName, uint32 variant)
        {
            if (profile.empty())
                return "";

            size_t pick = variant % 2;
            std::string motd;

            for (BespokeMotd const& bespoke : BESPOKE_MOTD)
            {
                if (profile.size() == 2 && profile[0] == bespoke.first && profile[1] == bespoke.second)
                {
                    motd = bespoke.templates[pick];
                    break;
                }
            }

            if (motd.empty())
            {
                motd = Copy(profile.front()).motdPrimary[pick];
                for (size_t i = 1; i < profile.size(); ++i)
                    motd += Copy(profile[i]).motdClause;
            }

            motd = Substitute(std::move(motd), guildName);
            if (motd.size() > MAX_MOTD_LENGTH)
                motd.resize(MAX_MOTD_LENGTH);
            return motd;
        }

        bool WouldColdPitch(FlavorProfile const& profile, uint8 targetLevel, uint8 maxLevel)
        {
            // A guild that levels has something for anyone. One that does not
            // is pitching endgame - battlegrounds, world PvP, raids - which a
            // lowbie cannot take up, so those bots wait for someone within
            // reach of it (roughly the last quarter of the climb: 56 of 80).
            if (profile.empty() || Has(profile, GuildFlavor::Leveling))
                return true;

            return targetLevel >= std::max<uint8>(1, uint8(uint32(maxLevel) * 7 / 10));
        }
    }

    GuildFlavorStore* GuildFlavorStore::instance()
    {
        static GuildFlavorStore instance;
        return &instance;
    }

    void GuildFlavorStore::Load()
    {
        QueryResult result = CharacterDatabase.Query("SELECT guildid, flavors FROM mod_llm_guild_flavor");
        if (!result)
            return;

        uint32 loaded = 0;
        std::lock_guard<std::mutex> lock(_mutex);
        do
        {
            Field* fields = result->Fetch();
            uint32 guildId = fields[0].Get<uint32>();
            std::string flavors = fields[1].Get<std::string>();

            FlavorProfile profile;
            if (!GuildFlavors::Deserialize(flavors, profile))
            {
                LOG_ERROR("module.llm", "Guild {} has an unreadable flavor profile '{}' - ignored",
                    guildId, flavors);
                continue;
            }

            _profiles[guildId] = std::move(profile);
            ++loaded;
        } while (result->NextRow());

        LOG_INFO("module.llm", "Loaded {} guild flavor profiles", loaded);
    }

    bool GuildFlavorStore::Get(uint32 guildId, FlavorProfile& out)
    {
        if (!guildId)
            return false;

        std::lock_guard<std::mutex> lock(_mutex);
        auto it = _profiles.find(guildId);
        if (it == _profiles.end())
            return false;

        out = it->second;
        return true;
    }

    void GuildFlavorStore::UpdateOnline()
    {
        if (!sLlmConfig->guildFlavorEnabled)
            return;

        // One pass over the online roster: a guild only matters here while
        // somebody who could talk about it is in world.
        std::vector<uint32> guildIds;
        for (auto const& [guid, player] : ObjectAccessor::GetPlayers())
        {
            uint32 guildId = player->IsInWorld() ? player->GetGuildId() : 0;
            if (guildId && std::find(guildIds.begin(), guildIds.end(), guildId) == guildIds.end())
                guildIds.push_back(guildId);
        }

        for (uint32 guildId : guildIds)
        {
            EnsureAssigned(guildId);

            // Only the leader's session may set a message of the day, so a
            // guild flavored while its leader was offline gets its MOTD on
            // the first pass after they log back in. A guild that has one
            // already is left alone for good.
            FlavorProfile profile;
            if (Get(guildId, profile))
                if (Guild* guild = sGuildMgr->GetGuildById(guildId))
                    StampMotd(guild, profile);
        }
    }

    void GuildFlavorStore::EnsureAssigned(uint32 guildId)
    {
        if (!guildId || !sLlmConfig->guildFlavorEnabled)
            return;

        {
            std::lock_guard<std::mutex> lock(_mutex);
            if (_profiles.contains(guildId))
                return;
        }

        Guild* guild = sGuildMgr->GetGuildById(guildId);
        if (!guild || !IsBotLed(guild))
            return;

        FlavorProfile profile;
        if (!Roll(profile))
            return;

        Remember(guildId, profile);
        Persist(guildId, profile);
        LOG_INFO("module.llm", "Guild <{}> ({}) flavored {}",
            guild->GetName(), guildId, GuildFlavors::Serialize(profile));

        StampMotd(guild, profile);
    }

    void GuildFlavorStore::Assign(uint32 guildId, FlavorProfile const& profile)
    {
        if (!guildId || profile.empty())
            return;

        Remember(guildId, profile);
        Persist(guildId, profile);
    }

    bool GuildFlavorStore::Reroll(uint32 guildId, FlavorProfile& out)
    {
        if (!guildId || !Roll(out))
            return false;

        Remember(guildId, out);
        Persist(guildId, out);
        return true;
    }

    void GuildFlavorStore::Forget(uint32 guildId)
    {
        {
            std::lock_guard<std::mutex> lock(_mutex);
            _profiles.erase(guildId);
        }

        CharacterDatabase.Execute("DELETE FROM mod_llm_guild_flavor WHERE guildid = {}", guildId);
    }

    void GuildFlavorStore::Clear()
    {
        std::lock_guard<std::mutex> lock(_mutex);
        _profiles.clear();
    }

    bool GuildFlavorStore::Roll(FlavorProfile& out)
    {
        uint32 total = GuildFlavors::TotalWeight(sLlmConfig->guildFlavorProfiles);
        if (!total)
        {
            LOG_ERROR("module.llm", "No usable LLM.GuildFlavor.Profiles entries - no guild gets a flavor");
            return false;
        }

        FlavorProfile const* rolled = GuildFlavors::Pick(sLlmConfig->guildFlavorProfiles, urand(0, total - 1));
        if (!rolled)
            return false;

        out = *rolled;
        return true;
    }

    void GuildFlavorStore::StampMotd(Guild* guild, FlavorProfile const& profile)
    {
        // Never overwrite: whatever a guild already says for itself wins, and
        // once stamped this is a no-op forever after.
        if (!guild->GetMOTD().empty())
            return;

        Player* leader = ObjectAccessor::FindPlayer(guild->GetLeaderGUID());
        if (!leader || !leader->GetSession())
            return;

        std::string motd = GuildFlavors::MotdFor(profile, guild->GetName(), urand(0, 1));
        if (motd.empty())
            return;

        guild->HandleSetMOTD(leader->GetSession(), motd);
        LOG_INFO("module.llm", "Guild <{}> ({}) message of the day set: '{}'",
            guild->GetName(), guild->GetId(), motd);
    }

    void GuildFlavorStore::Remember(uint32 guildId, FlavorProfile const& profile)
    {
        std::lock_guard<std::mutex> lock(_mutex);
        _profiles[guildId] = profile;
    }

    void GuildFlavorStore::Persist(uint32 guildId, FlavorProfile const& profile)
    {
        // Serialize() only ever emits [a-z+], so it needs no escaping.
        CharacterDatabase.Execute("REPLACE INTO mod_llm_guild_flavor (guildid, flavors) VALUES ({}, '{}')",
            guildId, GuildFlavors::Serialize(profile));
    }
}
