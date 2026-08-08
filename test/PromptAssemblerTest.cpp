/*
 * This file is part of mod-llm for Felworld. Released under the MIT license
 * (see the LICENSE file at the module root).
 */

#include "LlmConfig.h"
#include "PromptAssembler.h"
#include "gtest/gtest.h"

using namespace ModLlm;

namespace
{
    ContextSnapshot TestSnapshot()
    {
        ContextSnapshot snapshot;
        snapshot.botName = "Thundertusk";
        snapshot.botLevel = 30;
        snapshot.botClass = "shaman";
        snapshot.botRace = "troll";
        snapshot.botFaction = "Horde";
        snapshot.botArea = "Crossroads";
        snapshot.botZone = "The Barrens";
        snapshot.actorName = "Mera";
        snapshot.actorLevel = "level 28";
        snapshot.actorClass = "mage";
        snapshot.actorRace = "human";
        snapshot.channelLabel = "say";
        return snapshot;
    }

    TriggerContext TestTrigger()
    {
        TriggerContext trigger;
        trigger.kind = TRIGGER_CHAT_SAY;
        trigger.message = "hello there";
        return trigger;
    }

    void ResetTemplates()
    {
        sLlmConfig->promptSystem = "You are {bot_name}, level {bot_level} {bot_race} {bot_class}.";
        sLlmConfig->promptStyleExamples = "";
        sLlmConfig->promptChat = "{memory_block}{history_block}[{channel_label}] {actor_name}: \"{message}\"";
        sLlmConfig->promptEmote = "{actor_name} {message}.";
        sLlmConfig->promptEvent = "Event: {message}.";
        sLlmConfig->promptInitiative = "Idle. Around you: {environment}.";
        sLlmConfig->promptHistoryLine = "{speaker}: {message}";
    }
}

// cppcheck-suppress syntaxError
TEST(PromptAssemblerTest, BuildsSystemAndUserMessages)
{
    ResetTemplates();

    nlohmann::json messages = PromptAssembler::BuildMessages(TestSnapshot(), TestTrigger());

    ASSERT_TRUE(messages.is_array());
    ASSERT_EQ(messages.size(), 2u);
    EXPECT_EQ(messages[0]["role"], "system");
    EXPECT_EQ(messages[1]["role"], "user");

    std::string system = messages[0]["content"].get<std::string>();
    EXPECT_NE(system.find("Thundertusk"), std::string::npos);
    EXPECT_NE(system.find("troll shaman"), std::string::npos);

    std::string user = messages[1]["content"].get<std::string>();
    EXPECT_NE(user.find("[say] Mera"), std::string::npos);
    EXPECT_NE(user.find("hello there"), std::string::npos);
}

TEST(PromptAssemblerTest, StyleExamplesRenderedInSystemMessage)
{
    ResetTemplates();
    sLlmConfig->promptSystem = "You are {bot_name}.{style_examples}";
    sLlmConfig->promptStyleExamples = "\n\nHow you type:\nsomeone says thanks => np";

    std::string system = PromptAssembler::BuildMessages(TestSnapshot(), TestTrigger())[0]["content"].get<std::string>();
    EXPECT_NE(system.find("You are Thundertusk."), std::string::npos);
    EXPECT_NE(system.find("someone says thanks => np"), std::string::npos);
}

TEST(PromptAssemblerTest, MemoryBlockOnlyWhenPresent)
{
    ResetTemplates();

    ContextSnapshot snapshot = TestSnapshot();
    TriggerContext trigger = TestTrigger();

    std::string without = PromptAssembler::BuildMessages(snapshot, trigger)[1]["content"].get<std::string>();
    EXPECT_EQ(without.find("Your private notes:"), std::string::npos);

    snapshot.memoryBlock = "- [mera-friend] helped me clear the harpies\n";
    std::string with = PromptAssembler::BuildMessages(snapshot, trigger)[1]["content"].get<std::string>();
    EXPECT_NE(with.find("Your private notes:\n- [mera-friend] helped me clear the harpies"), std::string::npos);
}

TEST(PromptAssemblerTest, HistoryBlockIncluded)
{
    ResetTemplates();

    ContextSnapshot snapshot = TestSnapshot();
    snapshot.pairHistory = "Mera: hi\nThundertusk: yo\n";

    std::string user = PromptAssembler::BuildMessages(snapshot, TestTrigger())[1]["content"].get<std::string>();
    EXPECT_NE(user.find("Thundertusk: yo"), std::string::npos);
}

TEST(PromptAssemblerTest, OverheardHistoryIncludedFirst)
{
    ResetTemplates();

    ContextSnapshot snapshot = TestSnapshot();
    snapshot.overheardHistory = "Grok: anyone seen the caravan?\n";
    snapshot.pairHistory = "Mera: hi\n";

    std::string user = PromptAssembler::BuildMessages(snapshot, TestTrigger())[1]["content"].get<std::string>();
    size_t overheard = user.find("Recently overheard around you:\nGrok: anyone seen the caravan?");
    size_t pair = user.find("Your conversation with Mera:");
    ASSERT_NE(overheard, std::string::npos);
    ASSERT_NE(pair, std::string::npos);
    EXPECT_LT(overheard, pair);
}

TEST(PromptAssemblerTest, OverheardReachesInitiativeTemplate)
{
    ResetTemplates();
    sLlmConfig->promptInitiative = "{history_block}Idle. Around you: {environment}.";

    ContextSnapshot snapshot = TestSnapshot();
    snapshot.environment = "a kodo nearby";
    snapshot.overheardHistory = "Grok: north road is rough\n";

    TriggerContext trigger = TestTrigger();
    trigger.kind = TRIGGER_INITIATIVE;
    trigger.message.clear();

    std::string user = PromptAssembler::BuildMessages(snapshot, trigger)[1]["content"].get<std::string>();
    EXPECT_NE(user.find("Grok: north road is rough"), std::string::npos);
    EXPECT_NE(user.find("a kodo nearby"), std::string::npos);
}

TEST(PromptAssemblerTest, ReplyGuidanceRendered)
{
    ResetTemplates();
    sLlmConfig->promptChat = "{actor_name}: \"{message}\"{reply_guidance}";

    ContextSnapshot snapshot = TestSnapshot();
    snapshot.replyGuidance = " Everyone in the party heard this.";

    std::string user = PromptAssembler::BuildMessages(snapshot, TestTrigger())[1]["content"].get<std::string>();
    EXPECT_NE(user.find("hello there\" Everyone in the party heard this."), std::string::npos);
}

TEST(PromptAssemblerTest, TriggerKindSelectsTemplate)
{
    ResetTemplates();

    ContextSnapshot snapshot = TestSnapshot();
    snapshot.environment = "a kodo nearby";

    TriggerContext trigger = TestTrigger();
    trigger.kind = TRIGGER_INITIATIVE;
    trigger.message.clear();

    std::string user = PromptAssembler::BuildMessages(snapshot, trigger)[1]["content"].get<std::string>();
    EXPECT_NE(user.find("a kodo nearby"), std::string::npos);
}

TEST(PromptAssemblerTest, BadTemplateFallsBackInsteadOfThrowing)
{
    ResetTemplates();
    sLlmConfig->promptChat = "{this_placeholder_does_not_exist}";

    nlohmann::json messages;
    EXPECT_NO_THROW(messages = PromptAssembler::BuildMessages(TestSnapshot(), TestTrigger()));
    EXPECT_FALSE(messages[1]["content"].get<std::string>().empty());
}
