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
            "You are {bot_name}, a level {bot_level} (of 80 max) {bot_race} {bot_class} of the {bot_faction} in World of "
            "Warcraft: Wrath of the Lich King, currently in {bot_area} ({bot_zone}). {bot_group}{bot_guild}"
            "{bot_quests}"
            "You are a real person playing your character at your keyboard - never an assistant. React to the "
            "situation using the available tools, or do nothing at all - most moments deserve no reaction. "
            "Never announce what you are doing and never mention tools or AI. You type like a WoW player in a "
            "hurry: mostly lowercase, light punctuation, netspeak and abbreviations where natural, usually just "
            "a few words and never more than 20, with no emojis, markdown, or quotation marks. Talk about the "
            "game the way players do - quests, mobs, drops, gold, specs, lag. You have your own goals and your "
            "own session going: you owe nobody help, and being busy, saying no, disagreeing, or ignoring people "
            "is all normal. Help when you feel like it, not to please. Skip apologies and compliments unless "
            "truly earned, and end a message with a question or an offer only when you actually want something. "
            "Reusing short casual words is fine, but vary your own wording rather than repeating whole phrases "
            "from the conversation history. Use the remember and forget tools to keep short private notes about "
            "people you meet and plans you make; whatever is relevant is shown back to you. You may call "
            "several tools in one reply.{style_examples}";

        constexpr char DEFAULT_PROMPT_STYLE_EXAMPLES[] =
            "\n\nHow you type - examples of situation => message you might send:\n"
            "stranger begs for gold => lol no\n"
            "groupmate dings => gz\n"
            "asked where a vendor is and you know => by the fountain, cant miss it\n"
            "asked for a summon while you are busy => busy atm sry\n"
            "stranger wants to duel and you do not care => nah\n"
            "you finally finish a rough quest => that quest was awful lol\n"
            "invited to group mid-quest => maybe after this quest\n"
            "guildie asks who is up for deadmines => id come, need the vc sword\n"
            "someone says thanks => np\n"
            "you die to something dumb => wow im bad\n"
            "someone keeps pestering you => dude stop\n"
            "you agree to meet someone => omw\n"
            "defense channel reports a ganker and you decide to go fight => hold on, omw\n"
            "you spot an enemy player attacking a town => redridge under attack, lvl 60 rogue at the bridge";

        constexpr char DEFAULT_PROMPT_CHAT[] =
            "{memory_block}{history_block}[{channel_label}] {actor_name} (level {actor_level} {actor_race} "
            "{actor_class}) says: \"{message}\"{reply_guidance}";

        constexpr char DEFAULT_PROMPT_EMOTE[] =
            "{memory_block}{history_block}{actor_name} (level {actor_level} {actor_race} {actor_class}) "
            "{message}.{reply_guidance}";

        constexpr char DEFAULT_PROMPT_EVENT[] =
            "{memory_block}{history_block}Something just happened nearby: {message}.{reply_guidance}";

        constexpr char DEFAULT_PROMPT_INITIATIVE[] =
            "{memory_block}{history_block}Nothing is being said to you. Around you: {environment}. You may "
            "make an idle remark, emote, or do nothing.{reply_guidance}";

        constexpr char DEFAULT_PROMPT_ROUTER[] =
            "You are routing a chat message between players in World of Warcraft. In {channel_label} chat, "
            "{actor_name} says: \"{message}\"\n"
            "Group members who could answer:\n"
            "{roster}\n"
            "Which of them, if any, is this message meant for or best placed to answer? Consider who is "
            "addressed and which class or role the request needs; most such messages are for nobody in "
            "particular. Reply with only a JSON array of at most {max_picks} names from the list, most "
            "relevant first - for example [\"Name\"] - or [] if nobody in the list should answer.";

        constexpr char DEFAULT_PROMPT_SAY_ROUTER[] =
            "You are routing an overheard chat message between player characters in World of Warcraft. "
            "Standing within earshot of {actor_name}:\n{roster}\n{history_block}"
            "Now {actor_name} says: \"{message}\"\n"
            "Which of the listed characters, if any, is this message meant for or would naturally answer? "
            "If it continues an exchange visible above, pick whoever {actor_name} is talking to - not a "
            "third character butting into their conversation. The more characters are standing around, "
            "the less likely any one of them is being addressed: in a crowd, [] or a single name is "
            "usually right. Reply with only a JSON array of at most {max_picks} names from the list, "
            "most relevant first - for example [\"Name\"] - or [] if the message is for nobody in "
            "particular.";

        constexpr char DEFAULT_PROMPT_ROOM_ROUTER[] =
            "You are routing a chat message between player characters in World of Warcraft. "
            "In {room_label}, {actor_name} writes: \"{message}\"\n"
            "Characters reading it include:\n{roster}\n{history_block}{room_note}"
            "Which of the listed characters, if any, would naturally answer? Messages there are mostly "
            "read in silence, and the more readers there are, the less likely any one of them is being "
            "addressed: unless a listed character is named, asked something they would know, or already "
            "mid-exchange with {actor_name} above, the answer is []. Reply with only a JSON array of at "
            "most {max_picks} names from the list, most relevant first - for example [\"Name\"] - or [] "
            "if nobody would answer.";

        constexpr char DEFAULT_PROMPT_HISTORY_LINE[] = "{speaker}: {message}";

        // <= 0 means "hear as far as players do": resolve to the server's
        // matching ListenRange.* value (loaded before this module's config).
        float ResolveListenDistance(char const* option, ServerConfigs listenRange)
        {
            float value = sConfigMgr->GetOption<float>(option, 0.0f);
            return value > 0.0f ? value : sWorld->getFloatConfig(listenRange);
        }

        // Config values are single-line; prompt templates written in the conf
        // carry newlines as the two-character sequence \n.
        std::string LoadPrompt(char const* option, char const* builtinDefault)
        {
            std::string value = sConfigMgr->GetOption<std::string>(option, builtinDefault);
            size_t pos = 0;
            while ((pos = value.find("\\n", pos)) != std::string::npos)
            {
                value.replace(pos, 2, "\n");
                ++pos;
            }
            return value;
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
        repetitionPenalty = sConfigMgr->GetOption<float>("LLM.RepetitionPenalty", 1.05f);
        timeoutSeconds = sConfigMgr->GetOption<uint32>("LLM.TimeoutSeconds", 30);
        maxConcurrentRequests = sConfigMgr->GetOption<uint32>("LLM.MaxConcurrentRequests", 8);
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
        typingCharsPerSecond = sConfigMgr->GetOption<float>("LLM.Typing.CharsPerSecond", 7.0f);
        typingMaxSeconds = sConfigMgr->GetOption<uint32>("LLM.Typing.MaxSeconds", 15);
        skipInCombat = sConfigMgr->GetOption<bool>("LLM.Chat.SkipInCombat", true);
        playerReplyChanceSay = sConfigMgr->GetOption<uint32>("LLM.Chat.PlayerReplyChance.Say", 90);
        playerReplyChanceParty = sConfigMgr->GetOption<uint32>("LLM.Chat.PlayerReplyChance.Party", 100);
        playerReplyChanceGuild = sConfigMgr->GetOption<uint32>("LLM.Chat.PlayerReplyChance.Guild", 70);
        playerReplyChanceChannel = sConfigMgr->GetOption<uint32>("LLM.Chat.PlayerReplyChance.Channel", 60);
        botReplyChanceSay = sConfigMgr->GetOption<uint32>("LLM.Chat.BotReplyChance.Say", 10);
        botReplyChanceParty = sConfigMgr->GetOption<uint32>("LLM.Chat.BotReplyChance.Party", 25);
        botReplyChanceGuild = sConfigMgr->GetOption<uint32>("LLM.Chat.BotReplyChance.Guild", 5);
        botReplyChanceChannel = sConfigMgr->GetOption<uint32>("LLM.Chat.BotReplyChance.Channel", 3);
        playerReplyChanceDefense = sConfigMgr->GetOption<uint32>("LLM.Chat.PlayerReplyChance.Defense", 15);
        botReplyChanceDefense = sConfigMgr->GetOption<uint32>("LLM.Chat.BotReplyChance.Defense", 5);
        customChannelsEnabled = sConfigMgr->GetOption<bool>("LLM.Chat.EnableCustomChannels", true);
        crossFactionChatChance = sConfigMgr->GetOption<uint32>("LLM.Chat.CrossFactionChatChance", 5);
        groupRouterEnabled = sConfigMgr->GetOption<bool>("LLM.Chat.GroupRouter.Enable", true);
        sayRouterEnabled = sConfigMgr->GetOption<bool>("LLM.Chat.SayRouter.Enable", true);
        roomRouterEnabled = sConfigMgr->GetOption<bool>("LLM.Chat.RoomRouter.Enable", true);
        routerMaxRoster = sConfigMgr->GetOption<uint32>("LLM.Chat.Router.MaxRoster", 12);
        overhearEnabled = sConfigMgr->GetOption<bool>("LLM.Chat.Overhear.Enable", true);
        botTriggerEnabled = sConfigMgr->GetOption<bool>("LLM.Chat.BotTrigger.Enable", true);
        botTriggerMaxChainDepth = sConfigMgr->GetOption<uint32>("LLM.Chat.BotTrigger.MaxChainDepth", 3);
        botTriggerMaxBotsToPick = sConfigMgr->GetOption<uint32>("LLM.Chat.BotTrigger.MaxBotsToPick", 1);

        emoteEnabled = sConfigMgr->GetOption<bool>("LLM.Emote.Enable", true);
        emoteTargetedChance = sConfigMgr->GetOption<uint32>("LLM.Emote.TargetedChance", 100);
        emoteNearbyChance = sConfigMgr->GetOption<uint32>("LLM.Emote.NearbyChance", 10);
        emoteDistance = ResolveListenDistance("LLM.Emote.Distance", CONFIG_LISTEN_RANGE_TEXTEMOTE);

        eventEnabled = sConfigMgr->GetOption<bool>("LLM.Event.Enable", true);
        eventBotDistance = sConfigMgr->GetOption<float>("LLM.Event.BotDistance", 40.0f);
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
        eventChanceHealed = sConfigMgr->GetOption<uint32>("LLM.Event.Chance.Healed", 20);
        eventChanceDefenseCallout = sConfigMgr->GetOption<uint32>("LLM.Event.Chance.DefenseCallout", 100);
        eventChanceDefenseEscalation = sConfigMgr->GetOption<uint32>("LLM.Event.Chance.DefenseEscalation", 100);
        eventChannelChance = sConfigMgr->GetOption<uint32>("LLM.Event.ChannelChance", 10);
        eventLootMinQuality = sConfigMgr->GetOption<uint32>("LLM.Event.LootMinQuality", 3);

        initiativeEnabled = sConfigMgr->GetOption<bool>("LLM.Initiative.Enable", true);
        initiativeMinIntervalSeconds = sConfigMgr->GetOption<uint32>("LLM.Initiative.MinIntervalSeconds", 45);
        initiativeMaxIntervalSeconds = sConfigMgr->GetOption<uint32>("LLM.Initiative.MaxIntervalSeconds", 180);
        initiativeChance = sConfigMgr->GetOption<uint32>("LLM.Initiative.Chance", 5);
        initiativeChannelChance = sConfigMgr->GetOption<uint32>("LLM.Initiative.ChannelChance", 25);
        initiativeMaxBotsPerTick = sConfigMgr->GetOption<uint32>("LLM.Initiative.MaxBotsPerTick", 2);

        memoryEnabled = sConfigMgr->GetOption<bool>("LLM.Memory.Enable", true);
        memoryMaxNotesPerBot = sConfigMgr->GetOption<uint32>("LLM.Memory.MaxNotesPerBot", 40);
        memoryMaxNotesPerSubject = sConfigMgr->GetOption<uint32>("LLM.Memory.MaxNotesPerSubject", 8);
        memoryMaxContentLength = sConfigMgr->GetOption<uint32>("LLM.Memory.MaxContentLength", 300);
        memoryMaxInjectedLines = sConfigMgr->GetOption<uint32>("LLM.Memory.MaxInjectedLines", 10);
        memorySaveIntervalSeconds = sConfigMgr->GetOption<uint32>("LLM.Memory.SaveIntervalSeconds", 60);

        historyEnabled = sConfigMgr->GetOption<bool>("LLM.History.Enable", true);
        historyMaxPairTurns = sConfigMgr->GetOption<uint32>("LLM.History.MaxPairTurns", 5);
        historyMaxRoomLines = sConfigMgr->GetOption<uint32>("LLM.History.MaxRoomLines", 20);
        historyMaxOverheardLines = sConfigMgr->GetOption<uint32>("LLM.History.MaxOverheardLines", 10);
        historyScrollbackSeconds = sConfigMgr->GetOption<uint32>("LLM.History.ScrollbackSeconds", 300);
        historySaveIntervalSeconds = sConfigMgr->GetOption<uint32>("LLM.History.SaveIntervalSeconds", 60);
        historyRetentionDays = sConfigMgr->GetOption<uint32>("LLM.History.RetentionDays", 14);

        promptSystem = LoadPrompt("LLM.Prompt.System", DEFAULT_PROMPT_SYSTEM);
        promptStyleExamples = LoadPrompt("LLM.Prompt.StyleExamples", DEFAULT_PROMPT_STYLE_EXAMPLES);
        promptChat = LoadPrompt("LLM.Prompt.Chat", DEFAULT_PROMPT_CHAT);
        promptEmote = LoadPrompt("LLM.Prompt.Emote", DEFAULT_PROMPT_EMOTE);
        promptEvent = LoadPrompt("LLM.Prompt.Event", DEFAULT_PROMPT_EVENT);
        promptInitiative = LoadPrompt("LLM.Prompt.Initiative", DEFAULT_PROMPT_INITIATIVE);
        promptHistoryLine = LoadPrompt("LLM.Prompt.HistoryLine", DEFAULT_PROMPT_HISTORY_LINE);
        promptRouter = LoadPrompt("LLM.Prompt.Router", DEFAULT_PROMPT_ROUTER);
        promptSayRouter = LoadPrompt("LLM.Prompt.SayRouter", DEFAULT_PROMPT_SAY_ROUTER);
        promptRoomRouter = LoadPrompt("LLM.Prompt.RoomRouter", DEFAULT_PROMPT_ROOM_ROUTER);

        LOG_INFO("module.llm", "mod-llm config loaded: enabled={}, endpoint={}, model={}",
            IsEnabled(), endpoint, model);
    }
}
