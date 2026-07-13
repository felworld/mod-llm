/*
 * This file is part of mod-llm for Felworld. Released under the MIT license
 * (see the LICENSE file at the module root).
 */

#include "BotSelector.h"

#include "Channel.h"
#include "Containers.h"
#include "Group.h"
#include "Guild.h"
#include "LlmConfig.h"
#include "LlmTrigger.h"
#include "Map.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotMgr.h"
#include "Random.h"
#include "World.h"

#include <algorithm>
#include <cctype>

namespace ModLlm::BotSelector
{
    namespace
    {
        PlayerbotAI* GetBotAI(Player* player)
        {
            PlayerbotAI* botAI = sPlayerbotsMgr.GetPlayerbotAI(player);
            return botAI && botAI->IsBotAI() ? botAI : nullptr;
        }

        bool IsEligibleBot(Player* candidate, Player* sender)
        {
            if (!candidate || candidate == sender || !candidate->IsInWorld())
                return false;
            if (!GetBotAI(candidate))
                return false;
            if (sLlmConfig->skipInCombat && candidate->IsInCombat())
                return false;
            return true;
        }

        // Say/yell reaches the opposite faction only as untranslated gibberish,
        // so unless the server allows cross-faction chat a bot cannot
        // understand (or sensibly answer) the opposing team.
        bool CanUnderstand(Player* bot, Player* sender)
        {
            return bot->GetTeamId() == sender->GetTeamId()
                || sWorld->getBoolConfig(CONFIG_ALLOW_TWO_SIDE_INTERACTION_CHAT);
        }

        // The trigger must involve a human somewhere: either the sender is
        // one, or one is close enough to the bot to witness the reaction.
        bool HasHumanAudience(Player* bot, Player* sender, float distance)
        {
            if (IsRealPlayer(sender))
                return true;
            return HasRealPlayerNearby(bot, distance);
        }

        std::vector<Player*> PickByChanceAndMention(std::vector<Player*>& candidates, Player* sender,
            uint32 triggerKind, std::string const& message)
        {
            bool senderIsBot = !IsRealPlayer(sender);
            uint32 chance = ReplyChance(triggerKind, senderIsBot);

            std::vector<Player*> picked;

            // A bot addressed by name always reacts (first mention wins).
            for (Player* candidate : candidates)
            {
                if (MentionsName(message, candidate->GetName()))
                {
                    picked.push_back(candidate);
                    break;
                }
            }

            for (Player* candidate : candidates)
            {
                if (picked.size() >= sLlmConfig->maxBotsToPick)
                    break;
                if (!picked.empty() && picked[0] == candidate)
                    continue;
                if (urand(0, 99) < chance)
                    picked.push_back(candidate);
            }

            return picked;
        }
    }

    bool IsRealPlayer(Player* player)
    {
        return player && !GetBotAI(player);
    }

    bool HasRealPlayerNearby(Player* bot, float distance)
    {
        Map* map = bot->FindMap();
        if (!map)
            return false;

        for (MapReference const& ref : map->GetPlayers())
        {
            Player* player = ref.GetSource();
            if (player && IsRealPlayer(player) && bot->IsWithinDistInMap(player, distance))
                return true;
        }
        return false;
    }

    bool MentionsName(std::string const& message, std::string const& name)
    {
        if (name.empty() || message.size() < name.size())
            return false;

        auto lower = [](unsigned char c) { return char(std::tolower(c)); };

        std::string haystack(message.size(), '\0');
        std::transform(message.begin(), message.end(), haystack.begin(), lower);
        std::string needle(name.size(), '\0');
        std::transform(name.begin(), name.end(), needle.begin(), lower);

        size_t pos = 0;
        while ((pos = haystack.find(needle, pos)) != std::string::npos)
        {
            bool startOk = pos == 0 || std::isalpha(static_cast<unsigned char>(haystack[pos - 1])) == 0;
            size_t end = pos + needle.size();
            bool endOk = end >= haystack.size() || std::isalpha(static_cast<unsigned char>(haystack[end])) == 0;
            if (startOk && endOk)
                return true;
            ++pos;
        }
        return false;
    }

    uint32 ReplyChance(uint32 triggerKind, bool senderIsBot)
    {
        switch (triggerKind)
        {
            case TRIGGER_CHAT_WHISPER:
                return 100;
            case TRIGGER_CHAT_PARTY:
                return senderIsBot ? sLlmConfig->botReplyChanceParty : sLlmConfig->playerReplyChanceParty;
            case TRIGGER_CHAT_GUILD:
                return senderIsBot ? sLlmConfig->botReplyChanceGuild : sLlmConfig->playerReplyChanceGuild;
            case TRIGGER_CHAT_CHANNEL:
                return senderIsBot ? sLlmConfig->botReplyChanceChannel : sLlmConfig->playerReplyChanceChannel;
            default:
                return senderIsBot ? sLlmConfig->botReplyChanceSay : sLlmConfig->playerReplyChanceSay;
        }
    }

    std::vector<Player*> SelectForChat(Player* sender, uint32 triggerKind, std::string const& message,
        Group* group, Guild* guild, Channel* channel, float maxDistance)
    {
        std::vector<Player*> candidates;

        switch (triggerKind)
        {
            case TRIGGER_CHAT_SAY:
            {
                // Audience is proximity on the sender's map.
                Map* map = sender->FindMap();
                if (!map)
                    break;
                for (MapReference const& ref : map->GetPlayers())
                {
                    Player* player = ref.GetSource();
                    if (IsEligibleBot(player, sender) && CanUnderstand(player, sender)
                        && sender->IsWithinDistInMap(player, maxDistance)
                        && HasHumanAudience(player, sender, maxDistance))
                        candidates.push_back(player);
                }
                break;
            }
            case TRIGGER_CHAT_PARTY:
            {
                if (!group)
                    break;
                for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
                {
                    Player* player = ref->GetSource();
                    if (IsEligibleBot(player, sender)
                        && HasHumanAudience(player, sender, sLlmConfig->sayDistance))
                        candidates.push_back(player);
                }
                break;
            }
            case TRIGGER_CHAT_GUILD:
            {
                if (!guild)
                    break;
                // Iterate online players rather than the guild roster: cheaper
                // and only online members can react anyway.
                bool guildHasHuman = false;
                std::vector<Player*> guildBots;
                for (auto const& [guid, player] : ObjectAccessor::GetPlayers())
                {
                    if (!player->IsInWorld() || player->GetGuildId() != guild->GetId())
                        continue;
                    if (IsRealPlayer(player))
                        guildHasHuman = true;
                    else if (IsEligibleBot(player, sender))
                        guildBots.push_back(player);
                }
                if (guildHasHuman || IsRealPlayer(sender))
                    candidates = std::move(guildBots);
                break;
            }
            case TRIGGER_CHAT_CHANNEL:
            {
                if (!channel)
                    break;
                for (auto const& [guid, player] : ObjectAccessor::GetPlayers())
                {
                    if (!player->IsInWorld() || !player->IsInChannel(channel))
                        continue;
                    if (IsEligibleBot(player, sender)
                        && (IsRealPlayer(sender) || HasRealPlayerNearby(player, sLlmConfig->initiativeRealPlayerDistance)))
                        candidates.push_back(player);
                }
                break;
            }
            default:
                break;
        }

        if (candidates.empty())
            return candidates;

        Acore::Containers::RandomShuffle(candidates);
        return PickByChanceAndMention(candidates, sender, triggerKind, message);
    }

    std::vector<Player*> SelectNearby(Player* source, float distance, uint32 maxBots, bool includeSource)
    {
        std::vector<Player*> bots;

        Map* map = source->FindMap();
        if (!map)
            return bots;

        for (MapReference const& ref : map->GetPlayers())
        {
            Player* player = ref.GetSource();
            if (!player || !player->IsInWorld())
                continue;
            if (player == source && !includeSource)
                continue;
            if (player != source && !source->IsWithinDistInMap(player, distance))
                continue;
            if (!GetBotAI(player))
                continue;
            if (sLlmConfig->skipInCombat && player->IsInCombat())
                continue;
            bots.push_back(player);
        }

        Acore::Containers::RandomShuffle(bots);
        if (bots.size() > maxBots)
            bots.resize(maxBots);
        return bots;
    }
}
