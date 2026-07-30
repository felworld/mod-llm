/*
 * This file is part of mod-llm for Felworld. Released under the MIT license
 * (see the LICENSE file at the module root).
 */

#include "LlmTools.h"

#include "Bag.h"
#include "BotSelector.h"
#include "Channel.h"
#include "ChannelMgr.h"
#include "Chat.h"
#include "ChatHelper.h"
#include "GameTime.h"
#include "Group.h"
#include "Guild.h"
#include "HistoryStore.h"
#include "LlmConfig.h"
#include "LlmTrigger.h"
#include "Log.h"
#include "MemoryStore.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Overhear.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "PlayerbotMgr.h"
#include "SharedDefines.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "StringFormat.h"
#include "TextEmoteCatalog.h"
#include "ToolRegistry.h"
#include "Util.h"
#include "World.h"
#include "WorldPacket.h"
#include "WorldSession.h"

#include <algorithm>
#include <cctype>
#include <unordered_map>
#include <vector>

namespace ModLlm::LlmTools
{
    namespace
    {
        // Playerbots has no battleground-chat helper, so mirror its
        // SayToParty/SayToRaid: build the packet and hand it to every real
        // player in the group (other bots hear replies through history).
        bool SayToBattleground(ToolExecContext& context, std::string const& message)
        {
            if (!context.bot->GetGroup())
                return false;

            WorldPacket data;
            ChatHandler::BuildChatPacket(data, CHAT_MSG_BATTLEGROUND, message, LANG_UNIVERSAL,
                CHAT_TAG_NONE, context.bot->GetGUID(), context.bot->GetName());

            for (Player* receiver : context.ai->GetRealPlayersInGroup())
                receiver->GetSession()->SendPacket(&data);

            return true;
        }

        // Sends `message` into the exact channel named by the trigger.
        // PlayerbotAI::SayToChannel would re-resolve builtin channels by the
        // bot's current zone and report success even for a channel the bot
        // is not on, where Channel::Say quietly drops the line.
        bool SendToChannel(Player* bot, std::string const& channelName, std::string const& message)
        {
            if (ChannelMgr* mgr = ChannelMgr::forTeam(bot->GetTeamId()))
            {
                Channel* channel = mgr->GetChannel(channelName, bot, false);
                if (channel && bot->IsInChannel(channel))
                {
                    channel->Say(bot->GetGUID(), message, LANG_UNIVERSAL);
                    return true;
                }
            }
            return false;
        }

        // Speaking aloud (/say, /yell) is only worth doing when a human is
        // close enough to hear it, and only lands as words when the trigger's
        // actor shares a language (unless the cross-faction dice said the bot
        // may shout gibberish anyway).
        bool SpeakAloudBlocked(ToolExecContext& context, bool yelled, std::string& error)
        {
            TriggerContext const& trigger = *context.trigger;
            if (trigger.crossFaction && !trigger.crossFactionChatOk)
            {
                error = "you share no language with them; use the emote tool - its built-in"
                    " emotes carry across factions";
                return true;
            }

            float range = yelled ? sLlmConfig->yellDistance : sLlmConfig->sayDistance;
            if (!BotSelector::HasRealPlayerNearby(context.bot, range))
            {
                error = "nobody is close enough to hear you";
                return true;
            }
            return false;
        }

        // Sends `message` back into the channel the trigger came from. The
        // audience is bound by the trigger, never chosen by the model.
        bool RouteSay(ToolExecContext& context, std::string const& message, std::string& error)
        {
            TriggerContext const& trigger = *context.trigger;
            bool sent = false;

            // How the message went out, for the overhear fan-out below.
            bool spokeAloud = false; // /say or /yell, audible around the bot
            bool yelled = false;
            bool spokeInChannel = false;

            switch (trigger.kind)
            {
                case TRIGGER_CHAT_WHISPER:
                    sent = context.ai->Whisper(message, trigger.actorName);
                    break;
                case TRIGGER_CHAT_PARTY:
                    if (trigger.chatType == CHAT_MSG_BATTLEGROUND || trigger.chatType == CHAT_MSG_BATTLEGROUND_LEADER)
                        sent = SayToBattleground(context, message);
                    else if (trigger.chatType == CHAT_MSG_RAID || trigger.chatType == CHAT_MSG_RAID_LEADER
                        || trigger.chatType == CHAT_MSG_RAID_WARNING)
                        sent = context.ai->SayToRaid(message);
                    else
                        sent = context.ai->SayToParty(message);
                    break;
                case TRIGGER_CHAT_GUILD:
                    sent = context.ai->SayToGuild(message);
                    break;
                case TRIGGER_CHAT_CHANNEL:
                    sent = spokeInChannel = SendToChannel(context.bot, trigger.channelName, message);
                    break;
                default:
                    // Game events normally reply in /say, but a trigger can
                    // bind another audience (a group greeting on join, an
                    // initiative remark pointed at the zone channel).
                    if (trigger.chatType == CHAT_MSG_CHANNEL)
                        sent = spokeInChannel = SendToChannel(context.bot, trigger.channelName, message);
                    else if (trigger.chatType == CHAT_MSG_RAID)
                        sent = context.ai->SayToRaid(message);
                    else if (trigger.chatType == CHAT_MSG_PARTY)
                        sent = context.ai->SayToParty(message);
                    else
                    {
                        yelled = trigger.chatType == CHAT_MSG_YELL;
                        if (SpeakAloudBlocked(context, yelled, error))
                            return false;
                        sent = spokeAloud = yelled ? context.ai->Yell(message) : context.ai->Say(message);
                    }
                    break;
            }

            if (!sent)
            {
                error = "message could not be delivered";
                return false;
            }

            // History and overhear transcripts feed future prompts, so they
            // store what a player sees, never raw link markup.
            std::string plain = BotSelector::NormalizeChatLinks(message);

            if (trigger.actorGuid)
                sLlmHistoryStore->AddPairLine(trigger.botGuid, trigger.actorGuid, true,
                    context.bot->GetName(), plain);
            if (!trigger.roomKey.empty())
                sLlmHistoryStore->AddRoomLine(trigger.roomKey, trigger.botGuid,
                    context.bot->GetName(), plain);

            // Audible speech reaches bystander bots: they remember it and may
            // (config permitting) react to it.
            if (spokeAloud)
                Overhear::OnBotSpeech(context.bot, trigger, plain, yelled);
            else if (spokeInChannel)
                Overhear::OnBotChannelSpeech(context.bot, trigger, trigger.channelName, plain);

            return true;
        }

        // Finds a channel the bot is on by name; "General" matches the
        // zone-local "General - Durotar" so the model can use the short name.
        Channel* FindJoinedChannel(Player* bot, std::string const& name)
        {
            ChannelMgr* mgr = ChannelMgr::forTeam(bot->GetTeamId());
            if (!mgr)
                return nullptr;

            Channel* prefixMatch = nullptr;
            for (auto const& [key, channel] : mgr->GetChannels())
            {
                if (!channel || !bot->IsInChannel(channel))
                    continue;
                if (StringEqualI(channel->GetName(), name))
                    return channel;
                if (!prefixMatch && StringStartsWithI(channel->GetName(), name))
                    prefixMatch = channel;
            }
            return prefixMatch;
        }

        // Sends `message` to an explicitly chosen audience (the say tool's
        // `destination` argument): the one place the model may point a
        // message somewhere other than where the trigger came from. Every
        // branch validates the audience actually exists for this bot, and
        // logs history/overhear against the destination, not the trigger.
        bool RouteSayTo(ToolExecContext& context, std::string const& destination,
            std::string const& whisperTo, std::string const& channelName,
            std::string const& message, std::string& error)
        {
            TriggerContext const& trigger = *context.trigger;
            Player* bot = context.bot;

            // As in RouteSay: history and overhear lines store the visible
            // text of any links, not the markup.
            std::string plain = BotSelector::NormalizeChatLinks(message);

            if (destination == "say" || destination == "yell")
            {
                bool yelled = destination == "yell";
                if (SpeakAloudBlocked(context, yelled, error))
                    return false;
                if (!(yelled ? context.ai->Yell(message) : context.ai->Say(message)))
                {
                    error = "message could not be delivered";
                    return false;
                }
                Overhear::OnBotSpeech(bot, trigger, plain, yelled);
                return true;
            }

            if (destination == "party" || destination == "raid")
            {
                Group* group = bot->GetGroup();
                if (!group)
                {
                    error = "you are not in a group";
                    return false;
                }

                bool sent;
                if (group->isBGGroup() || group->isBFGroup())
                    sent = SayToBattleground(context, message);
                else if (destination == "raid")
                {
                    if (!group->isRaidGroup())
                    {
                        error = "your group is not a raid";
                        return false;
                    }
                    sent = context.ai->SayToRaid(message);
                }
                else
                    sent = context.ai->SayToParty(message);

                if (!sent)
                {
                    error = "message could not be delivered";
                    return false;
                }
                sLlmHistoryStore->AddRoomLine(
                    Acore::StringFormat("group:{}", group->GetGUID().GetCounter()),
                    trigger.botGuid, bot->GetName(), plain);
                return true;
            }

            if (destination == "guild")
            {
                Guild* guild = bot->GetGuild();
                if (!guild)
                {
                    error = "you are not in a guild";
                    return false;
                }
                if (!context.ai->SayToGuild(message))
                {
                    error = "message could not be delivered";
                    return false;
                }
                sLlmHistoryStore->AddRoomLine(Acore::StringFormat("guild:{}", guild->GetId()),
                    trigger.botGuid, bot->GetName(), plain);
                return true;
            }

            if (destination == "whisper")
            {
                std::string name = whisperTo;
                if (name.empty() || !normalizePlayerName(name))
                {
                    error = "whisper needs whisper_to set to a player name";
                    return false;
                }
                Player* receiver = ObjectAccessor::FindPlayerByName(name);
                if (!receiver)
                {
                    error = "no player with that name is online";
                    return false;
                }
                if (receiver == bot)
                {
                    error = "that is your own name";
                    return false;
                }
                if (!context.ai->Whisper(message, name))
                {
                    error = "you cannot whisper them";
                    return false;
                }
                sLlmHistoryStore->AddPairLine(trigger.botGuid, receiver->GetGUID(), true,
                    bot->GetName(), plain);
                Overhear::OnBotWhisper(bot, trigger, receiver, plain);
                return true;
            }

            if (destination == "channel")
            {
                if (channelName.empty())
                {
                    error = "channel needs channel_name";
                    return false;
                }
                Channel* channel = FindJoinedChannel(bot, channelName);
                if (!channel)
                {
                    error = "you are not on a channel with that name";
                    return false;
                }

                // Defense channels are alarm infrastructure, not an audience
                // the model may pick: the only speech that belongs there
                // arrives on the defense triggers themselves (callout events
                // and the go_defend-gated replies). This also catches
                // "World", which prefix-matches the joined "WorldDefense".
                // Swallowed, not failed: an error would invite the model to
                // retry the message, and silence is exactly the outcome the
                // channel wants.
                if (BotSelector::IsDefenseChannel(channel)
                    && !(trigger.defenseChannel && StringStartsWithI(channel->GetName(), trigger.channelName)))
                {
                    LOG_INFO("module.llm", "Bot {} say to '{}' swallowed: only defense triggers may speak"
                        " into a defense channel", bot->GetName(), channel->GetName());
                    return true;
                }

                channel->Say(bot->GetGUID(), message, LANG_UNIVERSAL);
                sLlmHistoryStore->AddRoomLine(
                    Acore::StringFormat("channel:{}:{}", channel->GetName(), uint32(bot->GetTeamId())),
                    trigger.botGuid, bot->GetName(), plain);
                Overhear::OnBotChannelSpeech(bot, trigger, channel->GetName(), plain);
                return true;
            }

            error = "unknown destination";
            return false;
        }

        // The next two helpers back both a tool's availability predicate (so
        // impossible actions are never offered to the model) and its executor
        // (state can change between prompt and execution).

        bool InviteBlocked(Player* bot, Player* actor, std::string& error)
        {
            if (bot->GetTeamId() != actor->GetTeamId())
            {
                error = "they are on the opposing faction";
                return true;
            }
            if (Group* group = bot->GetGroup())
            {
                if (actor->GetGroup() == group)
                {
                    error = "they are already in your group";
                    return true;
                }
                if (!group->IsLeader(bot->GetGUID()))
                {
                    error = "you are not the group leader";
                    return true;
                }
                if (group->IsFull())
                {
                    error = "your group is full";
                    return true;
                }
            }
            if (actor->GetGroup() || actor->GetGroupInvite())
            {
                error = "they are already in another group or have a pending invite";
                return true;
            }
            return false;
        }

        bool DuelBlocked(Player* bot, Player* actor, std::string& error)
        {
            if (!bot->IsAlive() || !actor->IsAlive())
            {
                error = "someone is dead";
                return true;
            }
            if (bot->duel || actor->duel)
            {
                error = "a duel is already pending";
                return true;
            }
            if (bot->IsInCombat() || actor->IsInCombat())
            {
                error = "someone is in combat";
                return true;
            }
            if (!bot->IsWithinDistInMap(actor, 10.0f))
            {
                error = "they are too far away";
                return true;
            }
            return false;
        }

        bool GuildInviteBlocked(Player* bot, Player* actor, std::string& error)
        {
            Guild* guild = bot->GetGuild();
            if (!guild)
            {
                error = "you are not in a guild";
                return true;
            }
            if (!guild->HasRankRight(bot, GR_RIGHT_INVITE))
            {
                error = "your guild rank cannot invite";
                return true;
            }
            if (!sWorld->getBoolConfig(CONFIG_ALLOW_TWO_SIDE_INTERACTION_GUILD)
                && bot->GetTeamId() != actor->GetTeamId())
            {
                error = "they are on the opposing faction";
                return true;
            }
            if (actor->GetGuildId())
            {
                error = "they are already in a guild";
                return true;
            }
            if (actor->GetGuildIdInvited())
            {
                error = "they already have a pending guild invite";
                return true;
            }
            return false;
        }

        bool FollowBlocked(Player* bot, PlayerbotAI* ai, Player* actor, std::string& error)
        {
            if (!bot->GetGroup() || bot->GetGroup() != actor->GetGroup())
            {
                error = "you are not in a group together";
                return true;
            }
            if (!ai || ai->GetMaster() != actor)
            {
                error = "you only follow the player leading you";
                return true;
            }
            return false;
        }

        // The friendly buffs a bot can put on another player, by class. The
        // first entry for a class is its signature buff, used when the model
        // does not name one.
        struct ClassBuff
        {
            uint8 playerClass;
            char const* name;
            uint32 firstRankSpellId;
        };

        constexpr ClassBuff CLASS_BUFFS[] =
        {
            { CLASS_MAGE,    "arcane intellect",      1459  },
            { CLASS_PRIEST,  "power word: fortitude", 1243  },
            { CLASS_PRIEST,  "divine spirit",         14752 },
            { CLASS_DRUID,   "mark of the wild",      1126  },
            { CLASS_DRUID,   "thorns",                467   },
            { CLASS_PALADIN, "blessing of might",     19740 },
            { CLASS_PALADIN, "blessing of wisdom",    19742 },
            { CLASS_PALADIN, "blessing of kings",     20217 },
            { CLASS_WARLOCK, "unending breath",       5697  },
        };

        // Every rank of `firstRank`'s chain the bot knows, ascending.
        std::vector<uint32> KnownRanks(Player* bot, uint32 firstRank)
        {
            std::vector<uint32> ranks;
            for (uint32 id = firstRank; id; id = sSpellMgr->GetNextSpellInChain(id))
                if (bot->HasSpell(id))
                    ranks.push_back(id);
            return ranks;
        }

        bool KnowsAnyBuff(Player* bot)
        {
            for (ClassBuff const& buff : CLASS_BUFFS)
                if (buff.playerClass == bot->getClass() && !KnownRanks(bot, buff.firstRankSpellId).empty())
                    return true;
            return false;
        }

        bool BuffBlocked(Player* bot, Player* actor, std::string& error)
        {
            if (bot->GetTeamId() != actor->GetTeamId())
            {
                error = "they are on the opposing faction";
                return true;
            }
            if (!bot->IsAlive() || !actor->IsAlive())
            {
                error = "someone is dead";
                return true;
            }
            if (!KnowsAnyBuff(bot))
            {
                error = "you have no buff spells";
                return true;
            }
            return false;
        }

        constexpr uint32 SPELL_RITUAL_OF_SUMMONING = 698;

        // The "Portal: <city>" spells the mage knows, as (spell id, city name).
        std::vector<std::pair<uint32, std::string>> KnownPortals(Player* bot)
        {
            std::vector<std::pair<uint32, std::string>> portals;
            for (auto const& [spellId, playerSpell] : bot->GetSpellMap())
            {
                if (playerSpell->State == PLAYERSPELL_REMOVED || !playerSpell->Active)
                    continue;

                SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(spellId);
                if (!spellInfo)
                    continue;

                std::string const name = spellInfo->SpellName[0];
                if (name.rfind("Portal: ", 0) != 0)
                    continue;

                portals.emplace_back(spellId, name.substr(8));
            }
            return portals;
        }

        // Class services (conjured refreshments, portals, summons) are favors
        // for the bot's own circle: groupmates, raidmates, and guildmates get
        // them for free. Battlegrounds are off-limits.
        bool ClassServiceBlocked(Player* bot, Player* actor, std::string& error)
        {
            if (bot->InBattleground() || actor->InBattleground())
            {
                error = "not inside a battleground";
                return true;
            }

            Group* group = bot->GetGroup();
            bool const grouped = group && group->IsMember(actor->GetGUID())
                && !group->isBGGroup() && !group->isBFGroup();
            bool const guilded = bot->GetGuildId() && bot->GetGuildId() == actor->GetGuildId();
            if (!grouped && !guilded)
            {
                error = "you only do that for groupmates and guildmates";
                return true;
            }
            return false;
        }

        bool ConjureBlocked(Player* bot, Player* actor, std::string& error)
        {
            if (ClassServiceBlocked(bot, actor, error))
                return true;
            if (bot->getClass() != CLASS_MAGE)
            {
                error = "you are not a mage";
                return true;
            }
            return false;
        }

        bool PortalBlocked(Player* bot, Player* actor, std::string& error)
        {
            if (ClassServiceBlocked(bot, actor, error))
                return true;
            if (bot->getClass() != CLASS_MAGE || KnownPortals(bot).empty())
            {
                error = "you have no portal spells";
                return true;
            }
            return false;
        }

        bool SummonBlocked(Player* bot, Player* actor, std::string& error)
        {
            if (ClassServiceBlocked(bot, actor, error))
                return true;
            if (bot->getClass() != CLASS_WARLOCK || !bot->HasSpell(SPELL_RITUAL_OF_SUMMONING))
            {
                error = "you cannot perform summoning rituals";
                return true;
            }

            Group* group = bot->GetGroup();
            if (!group || !group->IsMember(actor->GetGUID()))
            {
                error = "they must join your group before they can be summoned";
                return true;
            }
            return false;
        }

        bool GroupHasRealPlayer(Player* bot)
        {
            Group* group = bot->GetGroup();
            return group && BotSelector::GroupHasRealPlayer(group);
        }

        // Human-readable names for the get_gear listing, indexed by
        // EquipmentSlots.
        constexpr char const* EQUIPMENT_SLOT_NAMES[EQUIPMENT_SLOT_END] =
        {
            "head", "neck", "shoulders", "shirt", "chest", "waist", "legs", "feet",
            "wrists", "hands", "ring 1", "ring 2", "trinket 1", "trinket 2", "back",
            "main hand", "off hand", "ranged", "tabard"
        };

        // travel_to teleports like playerbots zone crossing does: only once no
        // human could watch the bot blink out, so it reads as "they made the
        // trip" rather than "they teleported". Pending trips are polled from
        // the world update. World thread only.
        constexpr float TRAVEL_HIDE_DISTANCE = 120.0f;
        constexpr time_t TRAVEL_EXPIRY_SECONDS = 600;

        struct PendingTravel
        {
            uint32 mapId;
            float x;
            float y;
            float z;
            float orientation;
            time_t expiry;
        };

        std::unordered_map<ObjectGuid, PendingTravel> pendingTravels;
    }

    std::string SanitizeChatText(std::string text)
    {
        // Cut anything that looks like leaked tool syntax.
        for (char const* marker : { "<tool_call>", "</tool_call>", "<|", "{\"name\":" })
        {
            size_t pos = text.find(marker);
            if (pos != std::string::npos)
                text.erase(pos);
        }

        // Trim whitespace and one layer of wrapping quotes.
        auto isTrimmable = [](unsigned char c) { return std::isspace(c) != 0; };
        while (!text.empty() && isTrimmable(text.front()))
            text.erase(text.begin());
        while (!text.empty() && isTrimmable(text.back()))
            text.pop_back();
        if (text.size() >= 2 && text.front() == '"' && text.back() == '"')
            text = text.substr(1, text.size() - 2);

        return text;
    }

    std::string ExpandChatLinks(std::string const& text,
        std::function<std::string(std::string const& kind, uint32 id)> const& resolve)
    {
        std::string result;
        result.reserve(text.size());

        size_t i = 0;
        while (i < text.size())
        {
            size_t open = text.find('{', i);
            if (open == std::string::npos)
            {
                result.append(text, i, std::string::npos);
                break;
            }
            result.append(text, i, open - i);

            // A link tag is exactly {kind:digits}; anything else keeps its
            // braces (models may legitimately write braces in prose).
            size_t colon = text.find(':', open + 1);
            size_t close = text.find('}', open + 1);
            bool isTag = colon != std::string::npos && close != std::string::npos && colon < close;
            std::string kind;
            std::string digits;
            if (isTag)
            {
                kind = text.substr(open + 1, colon - open - 1);
                digits = text.substr(colon + 1, close - colon - 1);
                std::transform(kind.begin(), kind.end(), kind.begin(),
                    [](unsigned char c) { return char(std::tolower(c)); });
                isTag = (kind == "item" || kind == "quest" || kind == "spell")
                    && !digits.empty() && digits.size() <= 9
                    && std::all_of(digits.begin(), digits.end(),
                        [](unsigned char c) { return std::isdigit(c) != 0; });
            }
            if (!isTag)
            {
                result += '{';
                i = open + 1;
                continue;
            }

            result += resolve(kind, uint32(std::stoul(digits)));
            i = close + 1;
        }

        return result;
    }

    std::string ExpandChatLinks(std::string const& text)
    {
        return ExpandChatLinks(text, [](std::string const& kind, uint32 id) -> std::string
        {
            if (kind == "item")
            {
                if (ItemTemplate const* proto = sObjectMgr->GetItemTemplate(id))
                    return ChatHelper::FormatItem(proto);
            }
            else if (kind == "quest")
            {
                if (Quest const* quest = sObjectMgr->GetQuestTemplate(id))
                    return ChatHelper::FormatQuest(quest);
            }
            else if (kind == "spell")
            {
                if (SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(id))
                    return ChatHelper::FormatSpell(spellInfo);
            }
            return "";
        });
    }

    void RegisterDefaultTools()
    {
        // say - available on every trigger. By default the audience is bound
        // by the trigger; the optional `destination` lets the model move a
        // message elsewhere (asked to pass something on to the guild, to
        // whisper a name, ...), strictly validated in RouteSayTo.
        sLlmToolRegistry->Register({
            "say",
            "Send a chat message to whoever you are currently talking with (the same channel the "
            "conversation is happening in). Link tags like {quest:844} or {item:12640}, copied "
            "verbatim from your quest log or a tool result, appear in chat as clickable links.",
            {
                { "type", "object" },
                { "properties", {
                    { "message", { { "type", "string" },
                        { "description", "The chat message to send, plain text; may include link "
                            "tags like {item:12640}" } } },
                    { "destination", { { "type", "string" },
                        { "enum", { "say", "yell", "party", "raid", "guild", "whisper", "channel" } },
                        { "description", "Where to send the message when it should go somewhere other "
                            "than the current conversation, e.g. when asked to tell your guild "
                            "something or to whisper someone. Omit to reply where you were spoken to." } } },
                    { "whisper_to", { { "type", "string" },
                        { "description", "Name of the player to whisper, with destination \"whisper\"" } } },
                    { "channel_name", { { "type", "string" },
                        { "description", "Channel to speak in, like General or Trade, with destination "
                            "\"channel\"" } } }
                } },
                { "required", { "message" } }
            },
            TRIGGER_ALL,
            false,
            [](ToolExecContext& context, nlohmann::json const& args, std::string& error)
            {
                // Expand after sanitizing (quote-stripping first), then trim
                // again: a message that was nothing but a dropped tag must
                // not go out as whitespace.
                std::string message = SanitizeChatText(
                    ExpandChatLinks(SanitizeChatText(args["message"].get<std::string>())));
                if (message.empty())
                {
                    error = "empty message";
                    return false;
                }

                std::string destination = args.value("destination", "");
                if (destination.empty())
                    return RouteSay(context, message, error);
                return RouteSayTo(context, destination, args.value("whisper_to", ""),
                    args.value("channel_name", ""), message, error);
            }
        });

        // emote - visible social animation.
        {
            nlohmann::json emoteNames = nlohmann::json::array();
            for (std::string const& name : TextEmoteCatalog::AllNames())
                emoteNames.push_back(name);

            sLlmToolRegistry->Register({
                "emote",
                "Perform a visible emote animation, like a real player using /wave or /laugh."
                " Only the listed built-in emotes exist.",
                {
                    { "type", "object" },
                    { "properties", { { "emote", { { "type", "string" }, { "enum", emoteNames } } } } },
                    { "required", { "emote" } }
                },
                TRIGGER_ALL,
                false,
                [](ToolExecContext& context, nlohmann::json const& args, std::string& error)
                {
                    uint32 emoteId = TextEmoteCatalog::FindId(args["emote"].get<std::string>());
                    if (!emoteId)
                    {
                        error = "unknown emote";
                        return false;
                    }
                    if (!context.ai->PlayEmote(emoteId))
                    {
                        error = "emote could not be performed";
                        return false;
                    }
                    return true;
                }
            });
        }

        // remember - upsert one note in the bot's private scratchpad.
        sLlmToolRegistry->Register({
            "remember",
            "Save a short private note to your memory; notes are shown back to you later. A note with "
            "the same slug is overwritten, so reuse a slug to update a note. Set about_player=true when "
            "the note is about the player you are interacting with, so it comes back whenever you meet "
            "them again. Use it for people you meet, promises, grudges, and plans.",
            {
                { "type", "object" },
                { "properties", {
                    { "slug", { { "type", "string" },
                        { "description", "Short id for the note, lowercase words joined by dashes" } } },
                    { "content", { { "type", "string" },
                        { "description", "The note itself, one or two short sentences" } } },
                    { "about_player", { { "type", "boolean" },
                        { "description", "True when the note is about the player you are interacting with" } } }
                } },
                { "required", { "slug", "content" } }
            },
            TRIGGER_ALL,
            false,
            [](ToolExecContext& context, nlohmann::json const& args, std::string& error)
            {
                if (!sLlmConfig->memoryEnabled)
                {
                    error = "memory is disabled";
                    return false;
                }

                uint32 subjectGuid = 0;
                if (args.value("about_player", false))
                {
                    if (!context.trigger->actorGuid)
                    {
                        error = "there is no player in this interaction to attach the note to";
                        return false;
                    }
                    subjectGuid = context.trigger->actorGuid.GetCounter();
                }

                error = sLlmMemoryStore->Upsert(context.trigger->botGuid,
                    args["slug"].get<std::string>(), subjectGuid, args["content"].get<std::string>());
                return error.empty();
            }
        });

        // forget - drop one note from the scratchpad.
        sLlmToolRegistry->Register({
            "forget",
            "Delete one of your private notes by its slug, for notes that are stale or no longer matter.",
            {
                { "type", "object" },
                { "properties", { { "slug", { { "type", "string" } } } } },
                { "required", { "slug" } }
            },
            TRIGGER_ALL,
            false,
            [](ToolExecContext& context, nlohmann::json const& args, std::string& error)
            {
                if (!sLlmConfig->memoryEnabled)
                {
                    error = "memory is disabled";
                    return false;
                }
                if (!sLlmMemoryStore->Forget(context.trigger->botGuid, args["slug"].get<std::string>()))
                {
                    error = "you have no note with that slug";
                    return false;
                }
                return true;
            }
        });

        // get_gear / get_inventory - read tools: instead of acting in the
        // world they hand data back to the model (via context.result and a
        // follow-up round), so a bot can answer "what are you wearing?" with
        // its real gear and link the pieces in chat.
        sLlmToolRegistry->Register({
            "get_gear",
            "Look up the gear you currently have equipped. Each piece is listed with a link tag "
            "like {item:12640}; copy a tag verbatim into a say message to show that item as a "
            "clickable link.",
            {
                { "type", "object" },
                { "properties", nlohmann::json::object() }
            },
            TRIGGER_ALL,
            false,
            [](ToolExecContext& context, nlohmann::json const& /*args*/, std::string& error)
            {
                std::string lines;
                for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
                {
                    Item* item = context.bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
                    if (!item)
                        continue;
                    lines += Acore::StringFormat("{}: {} {{item:{}}}\n",
                        EQUIPMENT_SLOT_NAMES[slot], item->GetTemplate()->Name1, item->GetEntry());
                }
                if (lines.empty())
                {
                    error = "you have nothing equipped";
                    return false;
                }
                context.result = "Your equipped gear:\n" + lines;
                return true;
            }
        });

        sLlmToolRegistry->Register({
            "get_inventory",
            "Look up what you are carrying in your bags, plus your money. Each item is listed with "
            "a link tag like {item:4306}; copy a tag verbatim into a say message to show that item "
            "as a clickable link.",
            {
                { "type", "object" },
                { "properties", nlohmann::json::object() }
            },
            TRIGGER_ALL,
            false,
            [](ToolExecContext& context, nlohmann::json const& /*args*/, std::string& /*error*/)
            {
                // Stacks of the same item aggregate into one line, in the
                // order they first appear in the bags.
                std::vector<std::pair<ItemTemplate const*, uint32>> counts;
                auto add = [&counts](Item* item)
                {
                    for (auto& [proto, count] : counts)
                        if (proto == item->GetTemplate())
                        {
                            count += item->GetCount();
                            return;
                        }
                    counts.emplace_back(item->GetTemplate(), item->GetCount());
                };

                for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
                    if (Item* item = context.bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
                        add(item);
                for (uint8 bagSlot = INVENTORY_SLOT_BAG_START; bagSlot < INVENTORY_SLOT_BAG_END; ++bagSlot)
                    if (Bag* bag = context.bot->GetBagByPos(bagSlot))
                        for (uint32 slot = 0; slot < bag->GetBagSize(); ++slot)
                            if (Item* item = bag->GetItemByPos(slot))
                                add(item);

                std::string lines;
                for (auto const& [proto, count] : counts)
                {
                    if (count > 1)
                        lines += Acore::StringFormat("{} x{} {{item:{}}}\n", proto->Name1, count, proto->ItemId);
                    else
                        lines += Acore::StringFormat("{} {{item:{}}}\n", proto->Name1, proto->ItemId);
                }

                context.result = lines.empty() ? "Your bags are empty. " : "In your bags:\n" + lines;
                context.result += Acore::StringFormat("You are carrying {}.",
                    ChatHelper::formatMoney(context.bot->GetMoney()));
                return true;
            }
        });

        // invite_to_party - synthetic client packet so all core validation runs.
        sLlmToolRegistry->Register({
            "invite_to_party",
            "Invite the player you are interacting with to join your party. Only use when they asked "
            "to group up or grouping clearly makes sense.",
            {
                { "type", "object" },
                { "properties", nlohmann::json::object() }
            },
            TRIGGER_CHAT_SAY | TRIGGER_CHAT_WHISPER | TRIGGER_CHAT_CHANNEL | TRIGGER_EMOTE | TRIGGER_GAME_EVENT,
            true,
            [](ToolExecContext& context, nlohmann::json const& /*args*/, std::string& error)
            {
                if (InviteBlocked(context.bot, context.actor, error))
                    return false;

                WorldPacket packet;
                packet << context.actor->GetName();
                packet << uint32(0); // roles mask
                context.bot->GetSession()->HandleGroupInviteOpcode(packet);
                return true;
            },
            [](Player* bot, Player* actor)
            {
                std::string ignored;
                return !InviteBlocked(bot, actor, ignored);
            }
        });

        // challenge_duel - casts the "Challenge to a Duel" spell; core validates.
        sLlmToolRegistry->Register({
            "challenge_duel",
            "Challenge the player you are interacting with to a duel. Only use when they asked for a "
            "duel or clearly provoked one.",
            {
                { "type", "object" },
                { "properties", nlohmann::json::object() }
            },
            TRIGGER_CHAT_SAY | TRIGGER_CHAT_WHISPER | TRIGGER_EMOTE,
            true,
            [](ToolExecContext& context, nlohmann::json const& /*args*/, std::string& error)
            {
                if (DuelBlocked(context.bot, context.actor, error))
                    return false;

                constexpr uint32 SPELL_DUEL_CHALLENGE = 7266;
                context.bot->CastSpell(context.actor, SPELL_DUEL_CHALLENGE, false);
                return true;
            },
            [](Player* bot, Player* actor)
            {
                std::string ignored;
                return !DuelBlocked(bot, actor, ignored);
            }
        });

        // roll - the classic /roll, visible to the bot's group.
        sLlmToolRegistry->Register({
            "roll",
            "Roll a random number like a player typing /roll (1-100 unless you pick another maximum). "
            "Your group sees the result; use it to settle loot or play a game of chance.",
            {
                { "type", "object" },
                { "properties", { { "max", { { "type", "integer" },
                    { "description", "Upper bound of the roll, default 100" } } } } }
            },
            TRIGGER_ALL,
            false,
            [](ToolExecContext& context, nlohmann::json const& args, std::string& error)
            {
                if (!context.bot->GetGroup())
                {
                    error = "you are not in a group, nobody would see the roll";
                    return false;
                }
                uint32 maxRoll = uint32(std::clamp<int64>(args.value("max", 100), 2, 1000000));
                context.bot->DoRandomRoll(1, maxRoll);
                return true;
            },
            [](Player* bot, Player* /*actor*/)
            {
                return bot && bot->GetGroup();
            }
        });

        // leave_party - the graceful exit; bots that linger forever feel fake.
        sLlmToolRegistry->Register({
            "leave_party",
            "Leave your current group, like a player who is done for the run. Say your goodbyes in "
            "the same reply.",
            {
                { "type", "object" },
                { "properties", nlohmann::json::object() }
            },
            TRIGGER_ALL,
            false,
            [](ToolExecContext& context, nlohmann::json const& /*args*/, std::string& error)
            {
                if (!context.bot->GetGroup())
                {
                    error = "you are not in a group";
                    return false;
                }
                if (context.bot->InBattleground())
                {
                    error = "you cannot leave a battleground group";
                    return false;
                }

                WorldPacket packet;
                context.bot->GetSession()->HandleGroupDisbandOpcode(packet);
                return true;
            },
            [](Player* bot, Player* /*actor*/)
            {
                return bot && bot->GetGroup() && !bot->InBattleground();
            }
        });

        // follow_player / stop_following - reuse the playerbots chat
        // shortcuts, so the follow behaves exactly like a master typing
        // "follow" / "stay".
        sLlmToolRegistry->Register({
            "follow_player",
            "Start following the player you are interacting with, like a player asked to 'follow me'.",
            {
                { "type", "object" },
                { "properties", nlohmann::json::object() }
            },
            TRIGGER_CHAT_SAY | TRIGGER_CHAT_WHISPER | TRIGGER_CHAT_PARTY | TRIGGER_EMOTE,
            true,
            [](ToolExecContext& context, nlohmann::json const& /*args*/, std::string& error)
            {
                if (FollowBlocked(context.bot, context.ai, context.actor, error))
                    return false;
                if (!context.ai->DoSpecificAction("follow chat shortcut", Event(), true))
                {
                    error = "could not start following";
                    return false;
                }
                return true;
            },
            [](Player* bot, Player* actor)
            {
                std::string ignored;
                return !FollowBlocked(bot, sPlayerbotsMgr.GetPlayerbotAI(bot), actor, ignored);
            }
        });

        sLlmToolRegistry->Register({
            "stop_following",
            "Stop following and stay where you are, like a player asked to 'wait here'.",
            {
                { "type", "object" },
                { "properties", nlohmann::json::object() }
            },
            TRIGGER_CHAT_SAY | TRIGGER_CHAT_WHISPER | TRIGGER_CHAT_PARTY | TRIGGER_EMOTE,
            true,
            [](ToolExecContext& context, nlohmann::json const& /*args*/, std::string& error)
            {
                if (FollowBlocked(context.bot, context.ai, context.actor, error))
                    return false;
                if (!context.ai->DoSpecificAction("stay chat shortcut", Event(), true))
                {
                    error = "could not stop";
                    return false;
                }
                return true;
            },
            [](Player* bot, Player* actor)
            {
                std::string ignored;
                return !FollowBlocked(bot, sPlayerbotsMgr.GetPlayerbotAI(bot), actor, ignored);
            }
        });

        // buff_player - cast a class buff on the actor, the iconic "can I
        // get fort?" interaction. Highest known rank, stepped down when the
        // target is too low-level for it.
        sLlmToolRegistry->Register({
            "buff_player",
            "Cast one of your class buffs on the player you are interacting with, like answering "
            "'buff me please'.",
            {
                { "type", "object" },
                { "properties", { { "buff", { { "type", "string" },
                    { "description", "Name of the buff they asked for. Omit to give your usual buff." } } } } }
            },
            TRIGGER_CHAT_SAY | TRIGGER_CHAT_WHISPER | TRIGGER_CHAT_PARTY | TRIGGER_CHAT_CHANNEL
                | TRIGGER_EMOTE | TRIGGER_GAME_EVENT,
            true,
            [](ToolExecContext& context, nlohmann::json const& args, std::string& error)
            {
                if (BuffBlocked(context.bot, context.actor, error))
                    return false;

                std::string wanted = args.value("buff", "");
                std::vector<uint32> ranks;
                for (ClassBuff const& buff : CLASS_BUFFS)
                {
                    if (buff.playerClass != context.bot->getClass())
                        continue;
                    if (!wanted.empty() && !StringContainsStringI(buff.name, wanted))
                        continue;
                    ranks = KnownRanks(context.bot, buff.firstRankSpellId);
                    if (!ranks.empty())
                        break;
                }
                if (ranks.empty())
                {
                    error = wanted.empty() ? "you have no buff spells" : "you do not know that buff";
                    return false;
                }

                SpellCastResult result = SPELL_CAST_OK;
                for (auto it = ranks.rbegin(); it != ranks.rend(); ++it)
                {
                    result = context.bot->CastSpell(context.actor, *it, false);
                    if (result != SPELL_FAILED_LOWLEVEL)
                        break;
                }
                switch (result)
                {
                    case SPELL_CAST_OK:
                        return true;
                    case SPELL_FAILED_OUT_OF_RANGE:
                    case SPELL_FAILED_LINE_OF_SIGHT:
                        error = "they are too far away";
                        return false;
                    case SPELL_FAILED_NO_POWER:
                        error = "you do not have enough mana";
                        return false;
                    case SPELL_FAILED_LOWLEVEL:
                        error = "they are too low level for your buff";
                        return false;
                    default:
                        error = "the spell did not work";
                        return false;
                }
            },
            [](Player* bot, Player* actor)
            {
                std::string ignored;
                return !BuffBlocked(bot, actor, ignored);
            }
        });

        // guild_invite - core's HandleInviteMember runs the full validation
        // and delivers the invite dialog; GuildInviteBlocked pre-checks so
        // the model gets a usable error instead of a silent no-op.
        sLlmToolRegistry->Register({
            "guild_invite",
            "Invite the player you are interacting with to join your guild. Only use when they asked "
            "to join or clearly want in.",
            {
                { "type", "object" },
                { "properties", nlohmann::json::object() }
            },
            TRIGGER_CHAT_SAY | TRIGGER_CHAT_WHISPER | TRIGGER_CHAT_CHANNEL | TRIGGER_EMOTE
                | TRIGGER_GAME_EVENT,
            true,
            [](ToolExecContext& context, nlohmann::json const& /*args*/, std::string& error)
            {
                if (GuildInviteBlocked(context.bot, context.actor, error))
                    return false;

                context.bot->GetGuild()->HandleInviteMember(context.bot->GetSession(),
                    context.actor->GetName());
                return true;
            },
            [](Player* bot, Player* actor)
            {
                std::string ignored;
                return !GuildInviteBlocked(bot, actor, ignored);
            }
        });

        // travel_to - the bot commits to a trip and teleports once nobody
        // could see it happen (UpdateTravel), so it looks like it walked.
        sLlmToolRegistry->Register({
            "travel_to",
            "Set out for a named place in the world, like a city, town, or zone. You make your own "
            "way and arrive after a while; you cannot bring anyone along.",
            {
                { "type", "object" },
                { "properties", { { "destination", { { "type", "string" },
                    { "description", "Place name, like Orgrimmar or Crossroads" } } } } },
                { "required", { "destination" } }
            },
            TRIGGER_ALL,
            false,
            [](ToolExecContext& context, nlohmann::json const& args, std::string& error)
            {
                GameTele const* tele = sObjectMgr->GetGameTele(args["destination"].get<std::string>());
                if (!tele)
                {
                    error = "you do not know the way there";
                    return false;
                }
                if (context.bot->GetMap()->Instanceable())
                {
                    error = "you cannot travel away from here";
                    return false;
                }
                if (GroupHasRealPlayer(context.bot))
                {
                    error = "you cannot wander off while grouped with someone";
                    return false;
                }
                if (tele->mapId == context.bot->GetMapId()
                    && context.bot->IsWithinDist3d(tele->position_x, tele->position_y, tele->position_z, 100.0f))
                {
                    error = "you are already there";
                    return false;
                }

                pendingTravels[context.trigger->botGuid] = { tele->mapId, tele->position_x,
                    tele->position_y, tele->position_z, tele->orientation,
                    GameTime::GetGameTime().count() + TRAVEL_EXPIRY_SECONDS };
                return true;
            },
            [](Player* bot, Player* /*actor*/)
            {
                return bot && !bot->GetMap()->Instanceable() && !GroupHasRealPlayer(bot);
            }
        });

        // go_defend - answer a defense-channel call for help by actually
        // going: hands the trip to playerbots' "wpvp defend" command, which
        // knows the reported ganker's position (defense board), simulates
        // travel time, and brings the bot home when the fight is over. Only
        // offered on defense-channel triggers, so "omw" and departure come
        // from the same decision.
        sLlmToolRegistry->Register({
            "go_defend",
            "Travel to a zone to fight off the reported enemy player, like actually answering a call "
            "for help in a defense channel. You make your own way there and it takes a while.",
            {
                { "type", "object" },
                { "properties", { { "zone", { { "type", "string" },
                    { "description", "Zone under attack, like Redridge Mountains. Omit when the call "
                        "is about this channel's own zone." } } } } }
            },
            TRIGGER_CHAT_CHANNEL,
            false,
            [](ToolExecContext& context, nlohmann::json const& args, std::string& error)
            {
                TriggerContext const& trigger = *context.trigger;
                if (!trigger.defenseChannel)
                {
                    error = "this is not a defense channel";
                    return false;
                }
                if (context.bot->GetMap()->Instanceable())
                {
                    error = "you cannot travel away from here";
                    return false;
                }
                if (GroupHasRealPlayer(context.bot))
                {
                    error = "you cannot run off while grouped with someone";
                    return false;
                }

                // LocalDefense channels are named "LocalDefense - <zone>":
                // an omitted zone means the trouble is right here.
                std::string zone = args.value("zone", "");
                if (zone.empty())
                {
                    size_t separator = trigger.channelName.find(" - ");
                    if (separator != std::string::npos)
                        zone = trigger.channelName.substr(separator + 3);
                }
                if (zone.empty())
                {
                    error = "say which zone needs defending";
                    return false;
                }

                if (!context.ai->DoSpecificAction("wpvp defend", Event("go_defend", zone, context.actor), true))
                {
                    error = "you do not know where to make a stand there";
                    return false;
                }
                return true;
            },
            nullptr,
            [](TriggerContext const& trigger)
            {
                return trigger.defenseChannel;
            }
        });

        // Class services - the mage/warlock favors playerbots already knows
        // how to perform (the !conjure / !portal / !ritual chat commands),
        // exposed to natural-language requests from the bot's own circle.
        // The playerbots actions own the mechanics: real casts, walking into
        // handover range, recruiting ritual helpers.
        sLlmToolRegistry->Register({
            "conjure_refreshments",
            "Conjure food or water and hand it to the player you are interacting with, like "
            "answering 'got any water?'. You cast the spell and walk over to trade the goods; "
            "it takes a few moments.",
            {
                { "type", "object" },
                { "properties", { { "kind", { { "type", "string" },
                    { "enum", { "food", "water" } } } } } },
                { "required", { "kind" } }
            },
            TRIGGER_CHAT_SAY | TRIGGER_CHAT_WHISPER | TRIGGER_CHAT_PARTY | TRIGGER_CHAT_GUILD
                | TRIGGER_CHAT_CHANNEL | TRIGGER_EMOTE,
            true,
            [](ToolExecContext& context, nlohmann::json const& args, std::string& error)
            {
                if (ConjureBlocked(context.bot, context.actor, error))
                    return false;
                if (!context.bot->IsWithinDistInMap(context.actor, sPlayerbotAIConfig.sightDistance))
                {
                    error = "they are too far away - they would have to come to you";
                    return false;
                }
                if (!context.ai->DoSpecificAction("conjure",
                    Event("llm", args["kind"].get<std::string>(), context.actor), true))
                {
                    error = "you cannot conjure that right now";
                    return false;
                }
                return true;
            },
            [](Player* bot, Player* actor)
            {
                std::string ignored;
                return !ConjureBlocked(bot, actor, ignored);
            }
        });

        sLlmToolRegistry->Register({
            "open_portal",
            "Open a mage portal to a capital city for the player you are interacting with, like "
            "answering 'can you port me to Ironforge?'. The portal opens at your feet and fades "
            "after a minute, so they must be standing near you.",
            {
                { "type", "object" },
                { "properties", { { "destination", { { "type", "string" },
                    { "description", "The city they asked for, like Stormwind or Undercity" } } } } },
                { "required", { "destination" } }
            },
            TRIGGER_CHAT_SAY | TRIGGER_CHAT_WHISPER | TRIGGER_CHAT_PARTY | TRIGGER_CHAT_GUILD
                | TRIGGER_CHAT_CHANNEL,
            true,
            [](ToolExecContext& context, nlohmann::json const& args, std::string& error)
            {
                if (PortalBlocked(context.bot, context.actor, error))
                    return false;

                std::string const wanted = args["destination"].get<std::string>();
                std::string city;
                for (auto const& [spellId, portalCity] : KnownPortals(context.bot))
                {
                    if (StringContainsStringI(portalCity, wanted))
                    {
                        city = portalCity;
                        break;
                    }
                }
                if (city.empty())
                {
                    error = "you can only open portals to:";
                    for (auto const& [spellId, portalCity] : KnownPortals(context.bot))
                        error += " " + portalCity + ",";
                    error.pop_back();
                    return false;
                }
                if (!context.bot->IsWithinDistInMap(context.actor, sPlayerbotAIConfig.sightDistance))
                {
                    error = "they are too far away to reach your portal - they would have to come to you";
                    return false;
                }
                if (!context.ai->DoSpecificAction("portal", Event("llm", city, context.actor), true))
                {
                    error = "you cannot open that portal right now";
                    return false;
                }
                return true;
            },
            [](Player* bot, Player* actor)
            {
                std::string ignored;
                return !PortalBlocked(bot, actor, ignored);
            }
        });

        sLlmToolRegistry->Register({
            "summon_player",
            "Perform a Ritual of Summoning to teleport the player you are interacting with to "
            "your location, like answering a groupmate's 'can I get a summon?'. Nearby helpers "
            "channel the portal with you and the player receives a summon request to accept.",
            {
                { "type", "object" },
                { "properties", nlohmann::json::object() }
            },
            TRIGGER_CHAT_SAY | TRIGGER_CHAT_WHISPER | TRIGGER_CHAT_PARTY | TRIGGER_CHAT_GUILD
                | TRIGGER_CHAT_CHANNEL,
            true,
            [](ToolExecContext& context, nlohmann::json const& /*args*/, std::string& error)
            {
                if (SummonBlocked(context.bot, context.actor, error))
                    return false;
                if (!context.ai->DoSpecificAction("ritual", Event("llm", "", context.actor), true))
                {
                    error = "the ritual cannot start - you need two group members or bystanders "
                        "nearby to help channel it";
                    return false;
                }
                return true;
            },
            [](Player* bot, Player* actor)
            {
                std::string ignored;
                return !SummonBlocked(bot, actor, ignored);
            }
        });

        LOG_INFO("module.llm", "Registered default LLM tools");
    }

    void UpdateTravel()
    {
        time_t now = GameTime::GetGameTime().count();
        for (auto it = pendingTravels.begin(); it != pendingTravels.end();)
        {
            Player* bot = ObjectAccessor::FindPlayer(it->first);

            // Conditions that void the trip entirely.
            if (!bot || !bot->IsInWorld() || it->second.expiry <= now
                || bot->GetMap()->Instanceable() || GroupHasRealPlayer(bot))
            {
                it = pendingTravels.erase(it);
                continue;
            }

            // Conditions that merely postpone it.
            if (!bot->IsAlive() || bot->IsInCombat() || bot->IsBeingTeleported()
                || BotSelector::HasRealPlayerNearby(bot, TRAVEL_HIDE_DISTANCE))
            {
                ++it;
                continue;
            }

            PendingTravel const& travel = it->second;
            bot->TeleportTo(travel.mapId, travel.x, travel.y, travel.z, travel.orientation);
            it = pendingTravels.erase(it);
        }
    }
}
