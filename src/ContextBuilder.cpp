/*
 * This file is part of mod-llm for Felworld. Released under the MIT license
 * (see the LICENSE file at the module root).
 */

#include "ContextBuilder.h"

#include "Battleground.h"
#include "BattlegroundWS.h"
#include "BotSelector.h"
#include "CellImpl.h"
#include "ChatHelper.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "Group.h"
#include "Guild.h"
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
            // model must retell or stay quiet. Trade ads carry their own
            // guidance in the trade-ad prompt instead.
            if ((trigger.kind == TRIGGER_INITIATIVE || trigger.kind == TRIGGER_GAME_EVENT)
                && !trigger.tradeAd
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

        // What the Warsong Gulch scoreboard shows a player - score and both
        // flags' status with carrier names - plus how teammates' play
        // callouts read. The model needs the facts to interpret a callout
        // ("inc" and "fc mid" mean different plays depending on whose flag is
        // where) before deciding on the bg_strategy tool. Empty outside an
        // active WSG match.
        std::string WsgGuidance(Player* bot)
        {
            Battleground* bg = bot->GetBattleground();
            if (!bg || bg->GetStatus() != STATUS_IN_PROGRESS)
                return "";

            BattlegroundTypeId bgType = bg->GetBgTypeID();
            if (bgType == BATTLEGROUND_RB)
                bgType = bg->GetBgTypeID(true);
            if (bgType != BATTLEGROUND_WS)
                return "";

            BattlegroundWS* ws = static_cast<BattlegroundWS*>(bg);
            TeamId myTeam = bot->GetTeamId();
            TeamId enemyTeam = myTeam == TEAM_ALLIANCE ? TEAM_HORDE : TEAM_ALLIANCE;

            auto carrierName = [](ObjectGuid guid) -> std::string
            {
                Player* carrier = ObjectAccessor::FindPlayer(guid);
                return carrier ? carrier->GetName() : "someone";
            };

            // Flag state and picker are indexed by the flag's owning team;
            // the picker is always on the other team.
            std::string myFlag;
            switch (ws->GetFlagState(myTeam))
            {
                case BG_WS_FLAG_STATE_ON_PLAYER:
                    myFlag = Acore::StringFormat("your flag was taken by enemy {}",
                        carrierName(ws->GetFlagPickerGUID(myTeam)));
                    break;
                case BG_WS_FLAG_STATE_ON_GROUND:
                    myFlag = "your flag is loose on the ground";
                    break;
                default:
                    myFlag = "your flag is safe in your base";
                    break;
            }

            std::string enemyFlag;
            switch (ws->GetFlagState(enemyTeam))
            {
                case BG_WS_FLAG_STATE_ON_PLAYER:
                    enemyFlag = Acore::StringFormat("your teammate {} is carrying the enemy flag",
                        carrierName(ws->GetFlagPickerGUID(enemyTeam)));
                    break;
                case BG_WS_FLAG_STATE_ON_GROUND:
                    enemyFlag = "the enemy flag is loose on the ground";
                    break;
                default:
                    enemyFlag = "the enemy flag sits in their base";
                    break;
            }

            std::string guidance = Acore::StringFormat(" You are mid-match in Warsong Gulch, first to 3"
                " flag captures wins. Score {}-{} in captures (you-them); {}; {}.",
                bg->GetTeamScore(myTeam), bg->GetTeamScore(enemyTeam), myFlag, enemyFlag);

            // A flag carrier's play is already decided - it never gets the
            // bg_strategy tool, so it gets marching orders instead of the
            // callout key.
            if (bot->HasAura(BG_WS_SPELL_WARSONG_FLAG) || bot->HasAura(BG_WS_SPELL_SILVERWING_FLAG))
                return guidance + " You are the one carrying the enemy flag: whatever gets called,"
                    " your job is getting it home alive.";

            return guidance
                + " Teammates call plays here in shorthand: inc means enemies incoming, at your flag room"
                " unless another spot is named, and fc points at a flag carrier, like fc mid or fc tunnel"
                " for where one is. When a callout deserves the team's attention, relay it with the"
                " bg_strategy tool - defend_base for inc at your base, attack_fc to hunt the enemy"
                " carrying your flag, defend_fc to stick with your carrier, attack_base to push their"
                " flag room. Whoever takes up the play announces it in this chat, so the tool call alone"
                " is a full response and staying otherwise silent is normal. You read the game yourself:"
                " relay the calls that make sense to you.";
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

        if (Guild* guild = sGuildMgr->GetGuildById(bot->GetGuildId()))
            snapshot.botGuild = Acore::StringFormat("You are a member of the guild <{}>. ", guild->GetName());

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

        // Battleground chat brings the scoreboard facts a player sees on
        // screen plus the key to reading play callouts (WSG only for now).
        if (trigger.chatType == CHAT_MSG_BATTLEGROUND || trigger.chatType == CHAT_MSG_BATTLEGROUND_LEADER)
            snapshot.replyGuidance += WsgGuidance(bot);

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
