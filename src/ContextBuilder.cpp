/*
 * This file is part of mod-llm for Felworld. Released under the MIT license
 * (see the LICENSE file at the module root).
 */

#include "ContextBuilder.h"

#include "CellImpl.h"
#include "ChatHelper.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "Group.h"
#include "Guild.h"
#include "GuildMgr.h"
#include "HistoryStore.h"
#include "LlmConfig.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotMgr.h"
#include "SentimentStore.h"
#include "SharedDefines.h"
#include "StringFormat.h"

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

        snapshot.actorName = actor ? actor->GetName() : trigger.actorName;
        if (actor)
        {
            snapshot.actorLevel = actor->GetLevel();
            snapshot.actorClass = ChatHelper::FormatClass(actor->getClass());
            snapshot.actorRace = ChatHelper::FormatRace(actor->getRace());
        }

        if (sLlmConfig->sentimentEnabled && trigger.actorGuid)
        {
            snapshot.hasSentiment = true;
            snapshot.sentimentValue = sLlmSentimentStore->Get(trigger.botGuid, trigger.actorGuid);
        }

        if (sLlmConfig->historyEnabled)
        {
            if (trigger.actorGuid)
                snapshot.pairHistory = sLlmHistoryStore->FormatPair(trigger.botGuid, trigger.actorGuid,
                    sLlmConfig->historyMaxPairTurns * 2);
            if (!trigger.roomKey.empty())
                snapshot.roomHistory = sLlmHistoryStore->FormatRoom(trigger.roomKey,
                    sLlmConfig->historyMaxRoomLines);
        }

        snapshot.channelLabel = ChannelLabel(trigger);
        snapshot.replyGuidance = ReplyGuidance(trigger);

        if (trigger.kind == TRIGGER_INITIATIVE)
            snapshot.environment = DescribeEnvironment(bot);

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
