/*
 * This file is part of mod-llm for Felworld. Released under the MIT license
 * (see the LICENSE file at the module root).
 */

#include "BotSelector.h"
#include "GameTime.h"
#include "Guild.h"
#include "GuildFlavor.h"
#include "HistoryStore.h"
#include "LlmClient.h"
#include "LlmConfig.h"
#include "LlmDispatch.h"
#include "LlmTools.h"
#include "MemoryStore.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "Random.h"
#include "ScriptMgr.h"
#include "TraceStore.h"
#include "TravelMgr.h"

#include <unordered_map>

namespace ModLlm
{
    namespace
    {
        // Bot-side gate for the guild-chatter initiative slices: in a guild,
        // with a rank that is actually allowed to invite.
        bool CanRecruitFor(Player* bot)
        {
            Guild* guild = bot->GetGuild();
            return guild && guild->HasRankRight(bot, GR_RIGHT_INVITE);
        }

        // A cold pitch only makes sense when the guild has something the
        // passerby can actually take up. A guild without the leveling tag is
        // selling endgame - battlegrounds, world PvP, raids - so it waits for
        // someone within reach of it instead of chatting up every lowbie.
        bool WorthPitchingTo(Player* bot, Player* target)
        {
            FlavorProfile profile;
            if (!sLlmConfig->guildFlavorEnabled || !sLlmGuildFlavors->Get(bot->GetGuildId(), profile))
                return true;

            return GuildFlavors::WouldColdPitch(profile, target->GetLevel(),
                uint8(sWorld->getIntConfig(CONFIG_MAX_PLAYER_LEVEL)));
        }
    }

    // Owns the module lifecycle (config, stores, worker pool) and the
    // initiative scheduler that lets bots act unprompted. Everything here runs
    // on the world thread.
    class LlmWorldScript : public WorldScript
    {
    public:
        LlmWorldScript() : WorldScript("LlmWorldScript", {
            WORLDHOOK_ON_AFTER_CONFIG_LOAD,
            WORLDHOOK_ON_STARTUP,
            WORLDHOOK_ON_UPDATE,
            WORLDHOOK_ON_SHUTDOWN
        }) { }

        void OnAfterConfigLoad(bool /*reload*/) override
        {
            sLlmConfig->Load();
        }

        void OnStartup() override
        {
            // Always bring the machinery up, even when LLM.Enable=0: the
            // worker threads idle and `.llm enable` can flip the switch at
            // runtime without a restart.
            LlmTools::RegisterDefaultTools();
            sLlmMemoryStore->Load();
            sLlmHistoryStore->Load();
            sLlmTraceStore->Load();
            sLlmGuildFlavors->Load();
            sLlmClient->Start();
        }

        void OnUpdate(uint32 diff) override
        {
            if (!sLlmConfig->IsEnabled())
            {
                Dispatch::ClearDelayed();
                return;
            }

            Dispatch::UpdateDelayed(diff);

            _initiativeTimer += diff;
            if (_initiativeTimer >= 5000)
            {
                _initiativeTimer = 0;
                UpdateInitiative();
            }

            // Lazy flavor assignment: a bot-led guild gets its profile (and,
            // once its leader is around, its message of the day) the first
            // time anybody in it is online with the module running.
            _guildFlavorTimer += diff;
            if (_guildFlavorTimer >= 10000)
            {
                _guildFlavorTimer = 0;
                sLlmGuildFlavors->UpdateOnline();
            }

            _travelTimer += diff;
            if (_travelTimer >= 3000)
            {
                _travelTimer = 0;
                LlmTools::UpdateTravel();
            }

            _memorySaveTimer += diff;
            if (_memorySaveTimer >= sLlmConfig->memorySaveIntervalSeconds * IN_MILLISECONDS)
            {
                _memorySaveTimer = 0;
                sLlmMemoryStore->SaveDirty();
            }

            _historySaveTimer += diff;
            if (_historySaveTimer >= sLlmConfig->historySaveIntervalSeconds * IN_MILLISECONDS)
            {
                _historySaveTimer = 0;
                sLlmHistoryStore->SaveDirty();
                sLlmTraceStore->SaveDirty();
            }
        }

        void OnShutdown() override
        {
            sLlmClient->Stop();
            sLlmMemoryStore->SaveDirty();
            sLlmHistoryStore->SaveDirty();
            sLlmTraceStore->SaveDirty();
        }

    private:
        void UpdateInitiative()
        {
            if (!sLlmConfig->initiativeEnabled)
                return;

            time_t now = GameTime::GetGameTime().count();
            uint32 submitted = 0;

            // The cold-pitch cooldown map stays tiny (one entry per recently
            // pitched human), so sweeping it every tick is cheap; afterwards
            // presence alone means "still on cooldown".
            std::erase_if(_recruitCooldown, [now](auto const& entry) { return entry.second <= now; });

            for (auto const& [guid, player] : ObjectAccessor::GetPlayers())
            {
                if (submitted >= sLlmConfig->initiativeMaxBotsPerTick)
                    break;
                if (!player->IsInWorld() || BotSelector::IsRealPlayer(player))
                    continue;

                auto [it, inserted] = _nextInitiative.try_emplace(guid.GetRawValue(), 0);
                if (inserted || !it->second)
                {
                    it->second = now + NextDelay();
                    continue;
                }
                if (it->second > now)
                    continue;

                it->second = now + NextDelay();

                if (urand(0, 99) >= sLlmConfig->initiativeChance)
                    continue;
                if (player->IsInCombat())
                    continue;

                TriggerContext trigger;
                trigger.kind = TRIGGER_INITIATIVE;

                // A slice of initiative fires becomes a market ad instead of
                // an idle remark: the prompt gets seeded with the bot's real
                // sellables and wants. Only bots actually standing in a
                // friendly capital advertise (playerbots keeps every bot on
                // the faction-wide Trade channel wherever it is, so channel
                // membership proves nothing) - mostly into Trade, with a
                // small share to zone General or plain /say, the way players
                // occasionally hawk outside Trade. When the ad cannot bind -
                // no human in the channel - fall through to the ordinary
                // idle remark path.
                if (urand(0, 99) < sLlmConfig->tradeAdChance &&
                    sTravelMgr.IsFriendlyCapital(player->GetZoneId(), player->GetTeamId()))
                {
                    TriggerContext ad;
                    if (BotSelector::BindTradeChannel(player, ad))
                    {
                        uint32 destination = urand(0, 99);
                        if (destination < sLlmConfig->tradeAdGeneralPercent)
                        {
                            TriggerContext general;
                            if (BotSelector::BindZoneChannel(player, general))
                                ad = general;  // else keep the Trade binding
                        }
                        else if (destination < sLlmConfig->tradeAdGeneralPercent + sLlmConfig->tradeAdSayPercent
                            && BotSelector::HasRealPlayerNearby(player, sLlmConfig->sayDistance))
                            ad = TriggerContext{};  // unbound: the reply goes out as /say

                        trigger = std::move(ad);
                        trigger.kind = TRIGGER_INITIATIVE;
                        trigger.tradeAd = true;
                    }
                }

                // Another slice becomes guild chatter, for bots whose guild
                // rank can actually invite. In a capital that is a
                // recruitment line into the city's GuildRecruitment channel -
                // the client auto-joins only unguilded players to it, so a
                // human on the channel is exactly a human who could be
                // recruited. Elsewhere it is the rarer cold pitch: the
                // closest passing unguilded player gets a friendly line and
                // (if the model commits) a real guild invite. Firing the
                // pitch puts that player on a cooldown shared by every bot -
                // set at fire time, so declining or ignoring one invite never
                // summons a parade of follow-up recruiters; a player who
                // *asks* to join is unaffected, that path is reactive.
                Player* recruit = nullptr;
                if (!trigger.tradeAd && CanRecruitFor(player))
                {
                    if (urand(0, 99) < sLlmConfig->guildAdChance
                        && sTravelMgr.IsFriendlyCapital(player->GetZoneId(), player->GetTeamId()))
                    {
                        TriggerContext ad;
                        if (BotSelector::BindGuildRecruitmentChannel(player, ad))
                        {
                            trigger = std::move(ad);
                            trigger.kind = TRIGGER_INITIATIVE;
                            trigger.guildAd = true;
                        }
                    }

                    if (!trigger.guildAd && !player->InBattleground()
                        && urand(0, 99) < sLlmConfig->guildRecruitChance)
                    {
                        recruit = BotSelector::FindRecruitTarget(player, sLlmConfig->sayDistance);
                        if (recruit && !WorthPitchingTo(player, recruit))
                            recruit = nullptr;
                        if (recruit && !_recruitCooldown.contains(recruit->GetGUID().GetRawValue()))
                        {
                            _recruitCooldown[recruit->GetGUID().GetRawValue()] =
                                now + sLlmConfig->guildRecruitCooldownSeconds;
                            trigger.guildRecruit = true;
                        }
                        else
                            recruit = nullptr;
                    }
                }

                // Some remarks go to the wide audience instead of /say - the
                // zone's General channel, or the team's chat while the bot is
                // in a battleground, which is where a match talks. The
                // human-audience gate widens to "anyone in that audience" to
                // match the wider reach; a /say remark needs a human close
                // enough to actually hear it.
                bool channelBound = trigger.chatType == CHAT_MSG_CHANNEL;
                if (!trigger.tradeAd && !trigger.guildAd && !trigger.guildRecruit)
                    channelBound = urand(0, 99) < sLlmConfig->initiativeChannelChance
                        && BotSelector::BindAmbientChannel(player, trigger);
                if (!channelBound
                    && !BotSelector::HasRealPlayerNearby(player, sLlmConfig->sayDistance))
                    continue;

                if (Dispatch::Submit(player, recruit, std::move(trigger)))
                    ++submitted;
            }
        }

        time_t NextDelay() const
        {
            return time_t(urand(sLlmConfig->initiativeMinIntervalSeconds,
                std::max(sLlmConfig->initiativeMinIntervalSeconds, sLlmConfig->initiativeMaxIntervalSeconds)));
        }

        uint32 _initiativeTimer = 0;
        uint32 _guildFlavorTimer = 0;
        uint32 _travelTimer = 0;
        uint32 _memorySaveTimer = 0;
        uint32 _historySaveTimer = 0;
        std::unordered_map<uint64, time_t> _nextInitiative;
        std::unordered_map<uint64, time_t> _recruitCooldown; // player guid -> pitchable again at
    };

    // Guild ids are handed out again after a disband, so a disbanded guild's
    // flavor has to go with it - otherwise the next guild to take that id
    // inherits an identity it never rolled.
    class LlmGuildScript : public GuildScript
    {
    public:
        LlmGuildScript() : GuildScript("LlmGuildScript", { GUILDHOOK_ON_DISBAND }) { }

        void OnDisband(Guild* guild) override
        {
            sLlmGuildFlavors->Forget(guild->GetId());
        }
    };
}

void AddSC_llm_world()
{
    new ModLlm::LlmWorldScript();
    new ModLlm::LlmGuildScript();
}
