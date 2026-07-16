/*
 * This file is part of mod-llm for Felworld. Released under the MIT license
 * (see the LICENSE file at the module root).
 */

#ifndef MOD_LLM_CONFIG_H
#define MOD_LLM_CONFIG_H

#include "Define.h"

#include <atomic>
#include <string>

namespace ModLlm
{
    // All LLM.* options, loaded once at startup and again on `.llm reload`.
    // Plain values; the worker threads only read them, so a reload while
    // requests are in flight is harmless (individual reads are atomic enough
    // for tuning knobs, and `enabled` is a real atomic).
    class LlmConfig
    {
    public:
        static LlmConfig* instance();

        void Load();

        bool IsEnabled() const { return enabled.load(std::memory_order_relaxed); }
        void SetEnabled(bool value) { enabled.store(value, std::memory_order_relaxed); }

        std::atomic<bool> enabled{false};

        // Endpoint / model
        std::string endpoint;
        std::string apiKey;
        std::string model;
        uint32 maxTokens = 200;
        // Negative (0 for topK) = not sent: the endpoint then applies the
        // model's own generation defaults (vLLM reads the model repo's
        // generation_config.json).
        float temperature = -1.0f;
        float topP = -1.0f;
        uint32 topK = 0;
        float repetitionPenalty = 1.1f;
        uint32 timeoutSeconds = 30;
        uint32 maxConcurrentRequests = 3;
        uint32 maxQueueSize = 32;
        bool treatBareContentAsSay = true;
        bool errorFeedbackEnabled = true;
        bool announceEnabled = true;
        bool debugEnabled = false;
        bool debugLogPrompts = false;

        // Reactive chat
        bool chatEnabled = true;
        bool whispersEnabled = true;
        // Resolved at Load: option value, or the server's matching
        // ListenRange.* when the option is <= 0 (bots hear as far as players).
        float sayDistance = 0.0f;
        float yellDistance = 0.0f;
        uint32 maxBotsToPick = 2;
        uint32 chatStaggerSeconds = 5;
        bool skipInCombat = true;
        uint32 playerReplyChanceSay = 90;
        uint32 playerReplyChanceParty = 100;
        uint32 playerReplyChanceGuild = 70;
        uint32 playerReplyChanceChannel = 60;
        uint32 botReplyChanceSay = 10;
        uint32 botReplyChanceParty = 25;
        uint32 botReplyChanceGuild = 5;
        uint32 botReplyChanceChannel = 3;
        bool customChannelsEnabled = true;
        bool groupRouterEnabled = true;
        bool sayRouterEnabled = true;
        bool overhearEnabled = true;

        // Bot speech triggering other bots (kept separate from the reply
        // chances so the whole mechanism can be flipped or tuned in one place)
        bool botTriggerEnabled = true;
        uint32 botTriggerMaxChainDepth = 2;

        // Emote reactions
        bool emoteEnabled = true;
        uint32 emoteTargetedChance = 100;
        uint32 emoteNearbyChance = 10;
        float emoteDistance = 0.0f; // <= 0 resolves to ListenRange.TextEmote

        // Game events
        bool eventEnabled = true;
        float eventBotDistance = 40.0f;
        float eventRealPlayerDistance = 40.0f;
        uint32 eventCooldownSeconds = 10;
        uint32 eventMaxBotsPerEvent = 2;
        uint32 eventChanceKill = 15;
        uint32 eventChancePvpKill = 40;
        uint32 eventChanceDeath = 30;
        uint32 eventChanceQuestComplete = 20;
        uint32 eventChanceLevelUp = 50;
        uint32 eventChanceDuel = 40;
        uint32 eventChanceAchievement = 40;
        uint32 eventChanceLoot = 15;
        uint32 eventChanceGroupJoin = 100;
        uint32 eventChannelChance = 10;
        uint32 eventLootMinQuality = 3; // ITEM_QUALITY_RARE

        // Initiative (unprompted idle behaviour)
        bool initiativeEnabled = true;
        uint32 initiativeMinIntervalSeconds = 45;
        uint32 initiativeMaxIntervalSeconds = 180;
        uint32 initiativeChance = 5;
        uint32 initiativeChannelChance = 25;
        float initiativeRealPlayerDistance = 200.0f;
        uint32 initiativeMaxBotsPerTick = 2;

        // Sentiment tracking
        bool sentimentEnabled = true;
        float sentimentDefault = 0.5f;
        float sentimentStepSmall = 0.05f;
        float sentimentStepLarge = 0.15f;
        uint32 sentimentSaveIntervalSeconds = 60;

        // Conversation history
        bool historyEnabled = true;
        uint32 historyMaxPairTurns = 5;
        uint32 historyMaxRoomLines = 20;
        uint32 historyMaxOverheardLines = 10;
        uint32 historySaveIntervalSeconds = 60;
        uint32 historyRetentionDays = 14;

        // Prompt templates (fmt named-argument placeholders)
        std::string promptSystem;
        std::string promptChat;
        std::string promptEmote;
        std::string promptEvent;
        std::string promptInitiative;
        std::string promptHistoryLine;
        std::string promptSentimentLine;
        std::string promptRouter;
        std::string promptSayRouter;
    };
}

#define sLlmConfig ModLlm::LlmConfig::instance()

#endif
