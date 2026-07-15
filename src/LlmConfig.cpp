/*
 * This file is part of mod-llm for Felworld. Released under the MIT license
 * (see the LICENSE file at the module root).
 */

#include "LlmConfig.h"

#include "Config.h"
#include "Log.h"
#include "World.h"

namespace ModLlm
{
    namespace
    {
        constexpr char DEFAULT_PROMPT_SYSTEM[] =
            "You are {bot_name}, a level {bot_level} {bot_race} {bot_class} of the {bot_faction} in World of "
            "Warcraft: Wrath of the Lich King, currently in {bot_area} ({bot_zone}). {bot_group}{bot_guild}"
            "You are a player character controlled by a real person, not an assistant. React to the situation "
            "using the available tools, or do nothing at all - most moments deserve no reaction. Never announce "
            "what you are doing and never mention tools or AI. Keep any chat message under 20 words, in a "
            "casual WoW-player tone with no emojis, markdown, or quotation marks. Vary your wording: never "
            "repeat a phrase that already appears in the conversation history, yours or anyone else's. You "
            "may call several tools in one reply.";

        constexpr char DEFAULT_PROMPT_CHAT[] =
            "{sentiment_line}{history_block}[{channel_label}] {actor_name} (level {actor_level} {actor_race} "
            "{actor_class}) says: \"{message}\"{reply_guidance}";

        constexpr char DEFAULT_PROMPT_EMOTE[] =
            "{sentiment_line}{history_block}{actor_name} (level {actor_level} {actor_race} {actor_class}) "
            "{message}.";

        constexpr char DEFAULT_PROMPT_EVENT[] =
            "{sentiment_line}{history_block}Something just happened nearby: {message}.{reply_guidance}";

        constexpr char DEFAULT_PROMPT_INITIATIVE[] =
            "{history_block}Nothing is being said to you. Around you: {environment}. You may make an idle "
            "remark, emote, or do nothing.{reply_guidance}";

        constexpr char DEFAULT_PROMPT_ROUTER[] =
            "You are routing a chat message between players in World of Warcraft. In {channel_label} chat, "
            "{actor_name} says: \"{message}\"\n"
            "Group members who could answer:\n"
            "{roster}\n"
            "Which of them, if any, is this message meant for or best placed to answer? Consider who is "
            "addressed and which class or role the request needs; most such messages are for nobody in "
            "particular. Reply with only a JSON array of at most {max_picks} names from the list, most "
            "relevant first - for example [\"Name\"] - or [] if nobody in the list should answer.";

        constexpr char DEFAULT_PROMPT_HISTORY_LINE[] = "{speaker}: {message}";

        constexpr char DEFAULT_PROMPT_SENTIMENT_LINE[] =
            "Your disposition toward {actor_name} is {sentiment_word} ({sentiment_value} on a 0..1 scale). ";

        // <= 0 means "hear as far as players do": resolve to the server's
        // matching ListenRange.* value (loaded before this module's config).
        float ResolveListenDistance(char const* option, ServerConfigs listenRange)
        {
            float value = sConfigMgr->GetOption<float>(option, 0.0f);
            return value > 0.0f ? value : sWorld->getFloatConfig(listenRange);
        }
    }

    LlmConfig* LlmConfig::instance()
    {
        static LlmConfig instance;
        return &instance;
    }

    void LlmConfig::Load()
    {
        SetEnabled(sConfigMgr->GetOption<bool>("LLM.Enable", false));

        endpoint = sConfigMgr->GetOption<std::string>("LLM.Endpoint", "http://ac-vllm:8000/v1/chat/completions");
        apiKey = sConfigMgr->GetOption<std::string>("LLM.ApiKey", "");
        model = sConfigMgr->GetOption<std::string>("LLM.Model", "felworld");
        maxTokens = sConfigMgr->GetOption<uint32>("LLM.MaxTokens", 200);
        temperature = sConfigMgr->GetOption<float>("LLM.Temperature", -1.0f);
        topP = sConfigMgr->GetOption<float>("LLM.TopP", -1.0f);
        topK = sConfigMgr->GetOption<uint32>("LLM.TopK", 0);
        repetitionPenalty = sConfigMgr->GetOption<float>("LLM.RepetitionPenalty", 1.1f);
        timeoutSeconds = sConfigMgr->GetOption<uint32>("LLM.TimeoutSeconds", 30);
        maxConcurrentRequests = sConfigMgr->GetOption<uint32>("LLM.MaxConcurrentRequests", 3);
        maxQueueSize = sConfigMgr->GetOption<uint32>("LLM.MaxQueueSize", 32);
        treatBareContentAsSay = sConfigMgr->GetOption<bool>("LLM.TreatBareContentAsSay", true);
        errorFeedbackEnabled = sConfigMgr->GetOption<bool>("LLM.ErrorFeedback.Enable", true);
        announceEnabled = sConfigMgr->GetOption<bool>("LLM.Announce.Enable", true);
        debugEnabled = sConfigMgr->GetOption<bool>("LLM.Debug.Enable", false);
        debugLogPrompts = sConfigMgr->GetOption<bool>("LLM.Debug.LogPrompts", false);

        chatEnabled = sConfigMgr->GetOption<bool>("LLM.Chat.Enable", true);
        whispersEnabled = sConfigMgr->GetOption<bool>("LLM.Chat.EnableWhispers", true);
        sayDistance = ResolveListenDistance("LLM.Chat.SayDistance", CONFIG_LISTEN_RANGE_SAY);
        yellDistance = ResolveListenDistance("LLM.Chat.YellDistance", CONFIG_LISTEN_RANGE_YELL);
        maxBotsToPick = sConfigMgr->GetOption<uint32>("LLM.Chat.MaxBotsToPick", 2);
        chatStaggerSeconds = sConfigMgr->GetOption<uint32>("LLM.Chat.StaggerSeconds", 5);
        skipInCombat = sConfigMgr->GetOption<bool>("LLM.Chat.SkipInCombat", true);
        playerReplyChanceSay = sConfigMgr->GetOption<uint32>("LLM.Chat.PlayerReplyChance.Say", 90);
        playerReplyChanceParty = sConfigMgr->GetOption<uint32>("LLM.Chat.PlayerReplyChance.Party", 100);
        playerReplyChanceGuild = sConfigMgr->GetOption<uint32>("LLM.Chat.PlayerReplyChance.Guild", 70);
        playerReplyChanceChannel = sConfigMgr->GetOption<uint32>("LLM.Chat.PlayerReplyChance.Channel", 60);
        botReplyChanceSay = sConfigMgr->GetOption<uint32>("LLM.Chat.BotReplyChance.Say", 10);
        botReplyChanceParty = sConfigMgr->GetOption<uint32>("LLM.Chat.BotReplyChance.Party", 25);
        botReplyChanceGuild = sConfigMgr->GetOption<uint32>("LLM.Chat.BotReplyChance.Guild", 5);
        botReplyChanceChannel = sConfigMgr->GetOption<uint32>("LLM.Chat.BotReplyChance.Channel", 3);
        customChannelsEnabled = sConfigMgr->GetOption<bool>("LLM.Chat.EnableCustomChannels", true);
        groupRouterEnabled = sConfigMgr->GetOption<bool>("LLM.Chat.GroupRouter.Enable", true);
        overhearEnabled = sConfigMgr->GetOption<bool>("LLM.Chat.Overhear.Enable", true);
        botTriggerEnabled = sConfigMgr->GetOption<bool>("LLM.Chat.BotTrigger.Enable", true);
        botTriggerMaxChainDepth = sConfigMgr->GetOption<uint32>("LLM.Chat.BotTrigger.MaxChainDepth", 2);

        emoteEnabled = sConfigMgr->GetOption<bool>("LLM.Emote.Enable", true);
        emoteTargetedChance = sConfigMgr->GetOption<uint32>("LLM.Emote.TargetedChance", 100);
        emoteNearbyChance = sConfigMgr->GetOption<uint32>("LLM.Emote.NearbyChance", 10);
        emoteDistance = ResolveListenDistance("LLM.Emote.Distance", CONFIG_LISTEN_RANGE_TEXTEMOTE);

        eventEnabled = sConfigMgr->GetOption<bool>("LLM.Event.Enable", true);
        eventBotDistance = sConfigMgr->GetOption<float>("LLM.Event.BotDistance", 40.0f);
        eventRealPlayerDistance = sConfigMgr->GetOption<float>("LLM.Event.RealPlayerDistance", 40.0f);
        eventCooldownSeconds = sConfigMgr->GetOption<uint32>("LLM.Event.CooldownSeconds", 10);
        eventMaxBotsPerEvent = sConfigMgr->GetOption<uint32>("LLM.Event.MaxBotsPerEvent", 2);
        eventChanceKill = sConfigMgr->GetOption<uint32>("LLM.Event.Chance.Kill", 15);
        eventChancePvpKill = sConfigMgr->GetOption<uint32>("LLM.Event.Chance.PvPKill", 40);
        eventChanceDeath = sConfigMgr->GetOption<uint32>("LLM.Event.Chance.Death", 30);
        eventChanceQuestComplete = sConfigMgr->GetOption<uint32>("LLM.Event.Chance.QuestComplete", 20);
        eventChanceLevelUp = sConfigMgr->GetOption<uint32>("LLM.Event.Chance.LevelUp", 50);
        eventChanceDuel = sConfigMgr->GetOption<uint32>("LLM.Event.Chance.Duel", 40);
        eventChanceAchievement = sConfigMgr->GetOption<uint32>("LLM.Event.Chance.Achievement", 40);
        eventChanceLoot = sConfigMgr->GetOption<uint32>("LLM.Event.Chance.Loot", 15);
        eventChanceGroupJoin = sConfigMgr->GetOption<uint32>("LLM.Event.Chance.GroupJoin", 100);
        eventChannelChance = sConfigMgr->GetOption<uint32>("LLM.Event.ChannelChance", 10);
        eventLootMinQuality = sConfigMgr->GetOption<uint32>("LLM.Event.LootMinQuality", 3);

        initiativeEnabled = sConfigMgr->GetOption<bool>("LLM.Initiative.Enable", true);
        initiativeMinIntervalSeconds = sConfigMgr->GetOption<uint32>("LLM.Initiative.MinIntervalSeconds", 45);
        initiativeMaxIntervalSeconds = sConfigMgr->GetOption<uint32>("LLM.Initiative.MaxIntervalSeconds", 180);
        initiativeChance = sConfigMgr->GetOption<uint32>("LLM.Initiative.Chance", 5);
        initiativeChannelChance = sConfigMgr->GetOption<uint32>("LLM.Initiative.ChannelChance", 25);
        initiativeRealPlayerDistance = sConfigMgr->GetOption<float>("LLM.Initiative.RealPlayerDistance", 200.0f);
        initiativeMaxBotsPerTick = sConfigMgr->GetOption<uint32>("LLM.Initiative.MaxBotsPerTick", 2);

        sentimentEnabled = sConfigMgr->GetOption<bool>("LLM.Sentiment.Enable", true);
        sentimentDefault = sConfigMgr->GetOption<float>("LLM.Sentiment.Default", 0.5f);
        sentimentStepSmall = sConfigMgr->GetOption<float>("LLM.Sentiment.StepSmall", 0.05f);
        sentimentStepLarge = sConfigMgr->GetOption<float>("LLM.Sentiment.StepLarge", 0.15f);
        sentimentSaveIntervalSeconds = sConfigMgr->GetOption<uint32>("LLM.Sentiment.SaveIntervalSeconds", 60);

        historyEnabled = sConfigMgr->GetOption<bool>("LLM.History.Enable", true);
        historyMaxPairTurns = sConfigMgr->GetOption<uint32>("LLM.History.MaxPairTurns", 5);
        historyMaxRoomLines = sConfigMgr->GetOption<uint32>("LLM.History.MaxRoomLines", 20);
        historyMaxOverheardLines = sConfigMgr->GetOption<uint32>("LLM.History.MaxOverheardLines", 10);
        historySaveIntervalSeconds = sConfigMgr->GetOption<uint32>("LLM.History.SaveIntervalSeconds", 60);
        historyRetentionDays = sConfigMgr->GetOption<uint32>("LLM.History.RetentionDays", 14);

        promptSystem = sConfigMgr->GetOption<std::string>("LLM.Prompt.System", DEFAULT_PROMPT_SYSTEM);
        promptChat = sConfigMgr->GetOption<std::string>("LLM.Prompt.Chat", DEFAULT_PROMPT_CHAT);
        promptEmote = sConfigMgr->GetOption<std::string>("LLM.Prompt.Emote", DEFAULT_PROMPT_EMOTE);
        promptEvent = sConfigMgr->GetOption<std::string>("LLM.Prompt.Event", DEFAULT_PROMPT_EVENT);
        promptInitiative = sConfigMgr->GetOption<std::string>("LLM.Prompt.Initiative", DEFAULT_PROMPT_INITIATIVE);
        promptHistoryLine = sConfigMgr->GetOption<std::string>("LLM.Prompt.HistoryLine", DEFAULT_PROMPT_HISTORY_LINE);
        promptSentimentLine = sConfigMgr->GetOption<std::string>("LLM.Prompt.SentimentLine", DEFAULT_PROMPT_SENTIMENT_LINE);
        promptRouter = sConfigMgr->GetOption<std::string>("LLM.Prompt.Router", DEFAULT_PROMPT_ROUTER);

        LOG_INFO("module.llm", "mod-llm config loaded: enabled={}, endpoint={}, model={}",
            IsEnabled(), endpoint, model);
    }
}
