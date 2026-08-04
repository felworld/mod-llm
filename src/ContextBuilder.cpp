/*
 * This file is part of mod-llm for Felworld. Released under the MIT license
 * (see the LICENSE file at the module root).
 */

#include "ContextBuilder.h"

#include "BattlegroundContext.h"
#include "BotSelector.h"
#include "CellImpl.h"
#include "ChatHelper.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "Group.h"
#include "Guild.h"
#include "GuildFlavor.h"
#include "GuildMgr.h"
#include "HistoryStore.h"
#include "LlmConfig.h"
#include "LlmTools.h"
#include "MemoryStore.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "PlayerbotMgr.h"
#include "QuestDef.h"
#include "SharedDefines.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "StringFormat.h"
#include "TradeOfferMgr.h"

namespace ModLlm::ContextBuilder
{
    namespace
    {
        std::string ChannelLabel(TriggerContext const& trigger)
        {
            switch (trigger.kind)
            {
                case TRIGGER_CHAT_SAY:
                    return trigger.chatType == CHAT_MSG_YELL ? "yell" : "say";
                case TRIGGER_CHAT_WHISPER:
                    return "whisper";
                case TRIGGER_CHAT_PARTY:
                    if (trigger.chatType == CHAT_MSG_PARTY || trigger.chatType == CHAT_MSG_PARTY_LEADER)
                        return "party";
                    if (trigger.chatType == CHAT_MSG_BATTLEGROUND || trigger.chatType == CHAT_MSG_BATTLEGROUND_LEADER)
                        return "battleground";
                    return "raid";
                case TRIGGER_CHAT_GUILD:
                    return "guild";
                case TRIGGER_CHAT_CHANNEL:
                    return trigger.channelName;
                default:
                    if (trigger.chatType == CHAT_MSG_CHANNEL && !trigger.channelName.empty())
                        return trigger.channelName;
                    if (trigger.chatType == CHAT_MSG_BATTLEGROUND)
                        return "battleground";
                    if (trigger.chatType == CHAT_MSG_RAID)
                        return "raid";
                    if (trigger.chatType == CHAT_MSG_PARTY)
                        return "party";
                    return "say";
            }
        }

        // Group channels reach every bot in the audience, so the model - not
        // a dice roll - decides who actually answers. The bigger the
        // audience, the harder the push toward silence.
        std::string ReplyGuidance(TriggerContext const& trigger)
        {
            // Defense channels: the reply rule is binary - a speaker is
            // coming (go_defend plus a short omw) or silent. First-hand
            // sightings arrive through the callout event path, not as
            // replies, and LlmToolOperation enforces this contract in code:
            // a channel-bound say without a successful go_defend beside it
            // is swallowed, so a model that types a decline anyway still
            // stays silent.
            if (trigger.kind == TRIGGER_CHAT_CHANNEL && trigger.defenseChannel)
                return Acore::StringFormat(" This is the \"{}\" defense channel, where people report enemy"
                    " attacks and call for help. The only message that belongs from you is a short"
                    " on-my-way sent together with the go_defend tool, when you actually go. Readers who"
                    " keep doing what they were doing say nothing at all, and that is almost always you.",
                    trigger.channelName);

            // An initiative remark or event comment pointed at the zone
            // channel: the audience is the whole zone, and none of them saw
            // what the bot just saw - a bare reaction lands as noise, so the
            // model must retell or stay quiet. Trade and guild ads carry
            // their own guidance in their prompts instead.
            if ((trigger.kind == TRIGGER_INITIATIVE || trigger.kind == TRIGGER_GAME_EVENT)
                && !trigger.tradeAd && !trigger.guildAd
                && trigger.chatType == CHAT_MSG_CHANNEL && !trigger.channelName.empty())
                return Acore::StringFormat(" If you say something, it goes to the zone-wide \"{}\" channel."
                    " Nobody there saw what just happened around you, so a remark only makes sense if you"
                    " tell them the story yourself - who and where, the way a player types \"some rogue"
                    " just ganked a lowbie by the crossroads lol\". Most local moments are not worth"
                    " broadcasting to a whole zone: staying silent is the usual choice.",
                    trigger.channelName);

            // The same remark or comment inside a battleground: the audience
            // is the bot's own team, and they are all in this match. Team
            // chat is the match's chat, so what belongs there is what is
            // happening here - not a story from the world outside.
            if ((trigger.kind == TRIGGER_INITIATIVE || trigger.kind == TRIGGER_GAME_EVENT)
                && trigger.chatType == CHAT_MSG_BATTLEGROUND)
                return " If you say something, it goes to your whole battleground team. They are in this"
                    " match with you: keep it to what is happening in here - the fight, the flags, the"
                    " score - and say nothing at all unless it is worth their time.";

            if (trigger.kind != TRIGGER_CHAT_PARTY)
                return "";

            if (trigger.chatType == CHAT_MSG_PARTY || trigger.chatType == CHAT_MSG_PARTY_LEADER)
                return " Everyone in the party heard this and someone else may answer."
                    " Reply only if it is meant for you or you have something worth saying; otherwise stay silent.";

            bool bg = trigger.chatType == CHAT_MSG_BATTLEGROUND || trigger.chatType == CHAT_MSG_BATTLEGROUND_LEADER;
            return Acore::StringFormat(" Everyone in the {} heard this. You are one voice among many:"
                " stay silent unless you are directly addressed or have something that truly needs saying.",
                bg ? "battleground" : "raid");
        }

        // Chat whose audience is the bot's own battleground team: team chat
        // itself, and the raid/party the match puts everyone in. A whisper or
        // a /say in the tunnel is a private conversation that happens to
        // occur in a battleground, so it does not pull in the scoreboard.
        bool TeamAudience(uint32 chatType)
        {
            switch (chatType)
            {
                case CHAT_MSG_BATTLEGROUND:
                case CHAT_MSG_BATTLEGROUND_LEADER:
                case CHAT_MSG_RAID:
                case CHAT_MSG_RAID_LEADER:
                case CHAT_MSG_RAID_WARNING:
                case CHAT_MSG_PARTY:
                case CHAT_MSG_PARTY_LEADER:
                    return true;
                default:
                    return false;
            }
        }
    }

    ContextSnapshot Build(Player* bot, Player* actor, TriggerContext const& trigger)
    {
        ContextSnapshot snapshot;

        snapshot.botName = bot->GetName();
        snapshot.botLevel = bot->GetLevel();
        snapshot.botClass = ChatHelper::FormatClass(bot->getClass());
        snapshot.botRace = ChatHelper::FormatRace(bot->getRace());
        snapshot.botFaction = bot->GetTeamId() == TEAM_ALLIANCE ? "Alliance" : "Horde";

        if (PlayerbotAI* botAI = sPlayerbotsMgr.GetPlayerbotAI(bot))
        {
            snapshot.botArea = PlayerbotAI::GetLocalizedAreaName(botAI->GetCurrentArea());
            snapshot.botZone = PlayerbotAI::GetLocalizedAreaName(botAI->GetCurrentZone());
        }

        // A player sees every group member's name in the party/raid frames,
        // so the bot gets the full roster too (and can tell that the person
        // it is talking to is already a groupmate).
        if (Group* group = bot->GetGroup())
        {
            std::string members;
            for (Group::MemberSlot const& slot : group->GetMemberSlots())
            {
                if (slot.guid == bot->GetGUID())
                    continue;
                if (!members.empty())
                    members += ", ";
                members += slot.name;
                if (slot.guid == group->GetLeaderGUID())
                    members += " (leader)";
            }

            char const* kind = group->isRaidGroup() ? "raid" : "party";
            if (members.empty())
                snapshot.botGroup = Acore::StringFormat("You are in a {} with nobody else in it yet. ", kind);
            else if (group->IsLeader(bot->GetGUID()))
                snapshot.botGroup = Acore::StringFormat("You lead a {} with {}. ", kind, members);
            else
                snapshot.botGroup = Acore::StringFormat("You are in a {} with {}. ", kind, members);
        }

        // A player knows what kind of guild they joined, so a member of a
        // flavored (bot-led) guild carries its identity into every prompt -
        // that clause is what makes one guild's chatter sound unlike the
        // next one's.
        FlavorProfile guildFlavor;
        bool flavored = false;
        if (Guild* guild = sGuildMgr->GetGuildById(bot->GetGuildId()))
        {
            flavored = sLlmConfig->guildFlavorEnabled && sLlmGuildFlavors->Get(guild->GetId(), guildFlavor);
            if (flavored)
                snapshot.botGuild = Acore::StringFormat("You are a member of <{}>, {}. ",
                    guild->GetName(), GuildFlavors::IdentityClause(guildFlavor));
            else
                snapshot.botGuild = Acore::StringFormat("You are a member of the guild <{}>. ", guild->GetName());
        }

        // A player always knows what is in their quest log, so the bot does
        // too - otherwise it invents quests it does not have.
        std::string quests;
        for (uint8 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
        {
            uint32 questId = bot->GetQuestSlotQuestId(slot);
            Quest const* quest = questId ? sObjectMgr->GetQuestTemplate(questId) : nullptr;
            if (!quest)
                continue;

            if (!quests.empty())
                quests += ", ";
            // The {quest:ID} tag is what the say tool expands into a
            // clickable quest link when the bot repeats it in chat.
            quests += Acore::StringFormat("\"{}\" {{quest:{}}}", quest->GetTitle(), questId);

            QuestStatus status = bot->GetQuestStatus(questId);
            if (status == QUEST_STATUS_COMPLETE)
                quests += " (ready to turn in)";
            else if (status == QUEST_STATUS_FAILED)
                quests += " (failed)";
        }
        if (!quests.empty())
            snapshot.botQuests = Acore::StringFormat("Your quest log: {}. ", quests);

        snapshot.actorName = actor ? actor->GetName() : trigger.actorName;
        if (actor)
        {
            snapshot.actorLevel = actor->GetLevel();
            snapshot.actorClass = ChatHelper::FormatClass(actor->getClass());
            snapshot.actorRace = ChatHelper::FormatRace(actor->getRace());
        }

        if (sLlmConfig->memoryEnabled)
            snapshot.memoryBlock = sLlmMemoryStore->Format(trigger.botGuid,
                trigger.actorGuid.GetCounter(), sLlmConfig->memoryMaxInjectedLines);

        if (sLlmConfig->historyEnabled)
        {
            if (trigger.actorGuid)
                snapshot.pairHistory = sLlmHistoryStore->FormatPair(trigger.botGuid, trigger.actorGuid,
                    sLlmConfig->historyMaxPairTurns * 2, snapshot.botName);
            if (!trigger.roomKey.empty())
                snapshot.roomHistory = sLlmHistoryStore->FormatRoom(trigger.roomKey,
                    sLlmConfig->historyMaxRoomLines, snapshot.botName);
            if (sLlmConfig->overhearEnabled)
                snapshot.overheardHistory = sLlmHistoryStore->FormatOverheard(trigger.botGuid,
                    sLlmConfig->historyMaxOverheardLines, snapshot.botName);
        }

        snapshot.channelLabel = ChannelLabel(trigger);
        snapshot.replyGuidance = ReplyGuidance(trigger);

        // Guild chat is the one room a guild's own identity sets the register
        // in: what gets talked about there, and in whose voice.
        if (flavored && trigger.kind == TRIGGER_CHAT_GUILD)
            snapshot.replyGuidance += GuildFlavors::ChatGuidance(guildFlavor);

        // Anything said to the bot's own team in a battleground brings the
        // scoreboard a player reads off their HUD, plus the key to the
        // shorthand teammates call plays in.
        if (TeamAudience(trigger.chatType))
            snapshot.replyGuidance += BattlegroundContext::Describe(bot);

        // No shared language across the faction line: steer the bot toward
        // the emote tool's built-in emotes - the only thing that carries
        // across factions. Free-typed action text does not exist as an emote
        // and typed chat arrives as gibberish (which the dice occasionally
        // make worth sending anyway).
        if (trigger.crossFaction)
        {
            char const* enemyFaction = bot->GetTeamId() == TEAM_ALLIANCE ? "Horde" : "Alliance";
            if (trigger.crossFactionChatOk)
                snapshot.replyGuidance += Acore::StringFormat(" {} is {} - you share no language."
                    " Only the emote tool's built-in emotes carry meaning across factions; anything"
                    " you type reaches them as gibberish, pure taunt value.",
                    snapshot.actorName, enemyFaction);
            else
                snapshot.replyGuidance += Acore::StringFormat(" {} is {} - you share no language,"
                    " and anything you type reaches them as unreadable gibberish. The emote tool's"
                    " built-in emotes are how you communicate: pick the one that fits.",
                    snapshot.actorName, enemyFaction);
        }

        if (trigger.kind == TRIGGER_INITIATIVE)
            snapshot.environment = DescribeEnvironment(bot);

        // A market ad is only as good as its grounding: the bot's actual
        // spare stock and actual wants, priced by the deterministic layer.
        if (trigger.tradeAd)
        {
            if (PlayerbotAI* botAI = sPlayerbotsMgr.GetPlayerbotAI(bot))
            {
                std::string lines;
                uint32 listed = 0;
                for (MarketQuote::Sellable const& sellable : MarketQuote::CollectSellables(botAI))
                {
                    if (++listed > 8)
                        break;
                    lines += Acore::StringFormat("selling: {} x{} {{item:{}}} - about {} each\n",
                        sellable.proto->Name1, sellable.count, sellable.proto->ItemId,
                        ChatHelper::formatMoney(sellable.askEach));
                }

                listed = 0;
                for (MarketQuote::Want const& want : MarketQuote::CollectWants(botAI))
                {
                    if (++listed > 6)
                        break;
                    lines += Acore::StringFormat("buying: {} {{item:{}}} - up to {} each\n",
                        want.proto->Name1, want.proto->ItemId, ChatHelper::formatMoney(want.bidEach));
                }

                // Class services the bot sells to strangers advertise
                // alongside the goods (the quote is jittered at deal time,
                // so the ad price stays approximate).
                if (ClassServices::SellsPortals(bot))
                {
                    std::string cities;
                    for (auto const& [spellId, city] : ClassServices::KnownPortals(bot))
                        cities += (cities.empty() ? "" : ", ") + city;
                    lines += Acore::StringFormat("offering: portals to {} - about {} a head\n",
                        cities, ChatHelper::formatMoney(sPlayerbotAIConfig.classServicePortalTip));
                }
                if (ClassServices::SellsSummons(bot))
                    lines += Acore::StringFormat(
                        "offering: summons to your spot here in {} - about {}\n",
                        PlayerbotAI::GetLocalizedAreaName(botAI->GetCurrentZone()),
                        ChatHelper::formatMoney(sPlayerbotAIConfig.classServiceSummonTip));

                snapshot.marketBlock = lines.empty() ? "nothing worth advertising right now" : lines;
            }
        }

        // Same grounding rule for guild chatter: the ad and the cold pitch
        // may only claim what is true - the guild's name, size, who is on,
        // and its own message of the day.
        if (trigger.guildAd || trigger.guildRecruit)
        {
            if (Guild* guild = sGuildMgr->GetGuildById(bot->GetGuildId()))
            {
                uint32 online = 0;
                for (auto const& [guid, player] : ObjectAccessor::GetPlayers())
                    if (player->IsInWorld() && player->GetGuildId() == guild->GetId())
                        ++online;

                snapshot.guildBlock = Acore::StringFormat("guild: <{}>\nmembers: {} ({} online now)",
                    guild->GetName(), guild->GetMemberCount(), online);
                if (!guild->GetMOTD().empty())
                    snapshot.guildBlock += Acore::StringFormat("\nmessage of the day: \"{}\"",
                        guild->GetMOTD());

                // What the guild is counts as a guild fact: the ad may claim
                // it, and the per-tag pitch guidance is what keeps one
                // guild's ads from reading like every other guild's.
                if (flavored)
                {
                    snapshot.guildBlock += "\n" + GuildFlavors::FlavorLine(guildFlavor);
                    snapshot.replyGuidance += GuildFlavors::RecruitGuidance(guildFlavor);
                }
            }
        }

        // What a player remembers over the last few minutes: they posted an
        // ad and are hanging around town for the bites - or they shook hands
        // on a deal and are on their way to close it. Keeps the model from
        // wandering off mid-negotiation via travel_to (the deterministic
        // dwell force underneath covers a model that ignores this anyway).
        PendingTradeDeal deal;
        bool hasDeal = sTradeOfferMgr->GetPending(bot->GetGUID(), deal);

        // A seller answering chat knows how its own trade works: the tool
        // call is what takes a job, and a bare "sure thing" in chat sets
        // nothing in motion (felworld/mod-llm#17). Standing knowledge, not
        // keyword-triggered - whether the message is actually asking is the
        // model's judgment. Skipped mid-deal: one deal at a time, and the
        // guidance below already says to see it through.
        constexpr uint32 chatKinds = TRIGGER_CHAT_SAY | TRIGGER_CHAT_WHISPER | TRIGGER_CHAT_PARTY
            | TRIGGER_CHAT_GUILD | TRIGGER_CHAT_CHANNEL;
        if (!hasDeal && (trigger.kind & chatKinds) && actor && BotSelector::IsRealPlayer(actor))
        {
            if (ClassServices::SellsPortals(bot))
                snapshot.replyGuidance += " You sell portals to the capitals for coin (groupmates and"
                    " guildmates ride free). Taking such a job means calling the open_portal tool with"
                    " the destination city - it sets up your price quote, the payment hand-off, and any"
                    " travel to the customer, and your chat reply then confirms what it reports back.";
            if (ClassServices::SellsSummons(bot))
                snapshot.replyGuidance += " You sell summons for coin (groupmates and guildmates ride"
                    " free), pulling a customer to wherever you are standing right now. Taking such a"
                    " job means calling the summon_player tool - it sets up your price quote, the group"
                    " invite, and the ritual, and your chat reply then confirms what it reports back.";
        }

        // An unguilded player talking to a bot whose rank can invite: the
        // reactive side of recruiting. A player knows what their own guild is
        // and pitches it honestly - no gatekeeping, anyone may ask.
        if (flavored && (trigger.kind & chatKinds) && actor && BotSelector::IsRealPlayer(actor)
            && !actor->GetGuildId())
        {
            Guild* guild = sGuildMgr->GetGuildById(bot->GetGuildId());
            if (guild && guild->HasRankRight(bot, GR_RIGHT_INVITE))
                snapshot.replyGuidance += GuildFlavors::InviteGuidance(guildFlavor);
        }

        if (hasDeal)
        {
            Player* counterparty = ObjectAccessor::FindPlayer(deal.counterpartyGuid);
            std::string who = counterparty ? counterparty->GetName() : "a customer";
            std::string money = ChatHelper::formatMoney(deal.price);
            if (deal.service == TradeService::Summon)
                snapshot.replyGuidance += Acore::StringFormat(" You have a paid summon going for {} -"
                    " {} collected in a trade once they land here. Stay put and see it through.",
                    who, money);
            else if (deal.service == TradeService::Portal)
            {
                std::string what = "a portal";
                if (SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(deal.serviceSpellId))
                    if (std::string(spellInfo->SpellName[0]).rfind("Portal: ", 0) == 0)
                        what = "a portal to " + std::string(spellInfo->SpellName[0]).substr(8);
                if (deal.departAt && !deal.teleported)
                    snapshot.replyGuidance += Acore::StringFormat(" You just shook hands with {} on {}"
                        " for {} and are making your way over to them in another town - it takes a few"
                        " minutes, no need to narrate the trip. The coin changes hands in a trade"
                        " before you cast.", who, what, money);
                else
                    snapshot.replyGuidance += Acore::StringFormat(" You have a deal going with {} -"
                        " {} for {}, coin first through a trade window - and are heading over to close"
                        " it. Stay on that.", who, what, money);
            }
            else
            {
                ItemTemplate const* dealProto = sObjectMgr->GetItemTemplate(deal.itemId);
                std::string what = dealProto ? dealProto->Name1 : "goods";
                if (deal.departAt && !deal.teleported)
                    snapshot.replyGuidance += Acore::StringFormat(" You just shook hands on a deal and are"
                        " making your way over to {} in another town to {} {} - it takes a few minutes,"
                        " no need to narrate the trip.", who, deal.selling ? "hand over" : "buy", what);
                else
                    snapshot.replyGuidance += Acore::StringFormat(" You have a deal going with {} to {} {}"
                        " and are heading over to close it - stay on that.",
                        who, deal.selling ? "hand over" : "buy", what);
            }
        }
        else if (uint32 anchorLeft = sTradeOfferMgr->AnchorSecondsLeft(bot->GetGUID()))
            snapshot.replyGuidance += Acore::StringFormat(" You recently put out trade chatter and are"
                " sticking around town for another {} minutes or so in case someone bites.",
                std::max<uint32>(1, (anchorLeft + 30) / 60));

        return snapshot;
    }

    std::string DescribeEnvironment(Player* bot)
    {
        std::string description;

        if (PlayerbotAI* botAI = sPlayerbotsMgr.GetPlayerbotAI(bot))
        {
            std::string area = PlayerbotAI::GetLocalizedAreaName(botAI->GetCurrentArea());
            if (!area.empty())
                description += area;
        }

        // Nearest non-player creature for a bit of local flavour.
        std::list<Unit*> units;
        Acore::AnyUnitInObjectRangeCheck check(bot, 30.0f);
        Acore::UnitListSearcher<Acore::AnyUnitInObjectRangeCheck> searcher(bot, units, check);
        Cell::VisitObjects(bot, searcher, 30.0f);

        for (Unit* unit : units)
        {
            if (!unit->IsCreature() || unit->IsPet() || unit->ToCreature()->IsTrigger())
                continue;

            if (!description.empty())
                description += "; ";
            description += Acore::StringFormat("a {} nearby", unit->GetName());
            break;
        }

        if (description.empty())
            description = "nothing of note";

        return description;
    }
}
