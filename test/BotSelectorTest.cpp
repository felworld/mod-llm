/*
 * This file is part of mod-llm for Felworld. Released under the MIT license
 * (see the LICENSE file at the module root).
 */

#include "BotSelector.h"
#include "LlmConfig.h"
#include "LlmTrigger.h"
#include "gtest/gtest.h"

using namespace ModLlm;

// cppcheck-suppress syntaxError
TEST(BotSelectorTest, MentionMatchesWholeWordCaseInsensitive)
{
    EXPECT_TRUE(BotSelector::MentionsName("hey Thrall, got a sec?", "thrall"));
    EXPECT_TRUE(BotSelector::MentionsName("THRALL come here", "Thrall"));
    EXPECT_TRUE(BotSelector::MentionsName("thrall", "Thrall"));
}

TEST(BotSelectorTest, MentionRejectsSubstrings)
{
    EXPECT_FALSE(BotSelector::MentionsName("enthralled by the view", "Thrall"));
    EXPECT_FALSE(BotSelector::MentionsName("thralls everywhere", "Thrall"));
    EXPECT_FALSE(BotSelector::MentionsName("", "Thrall"));
}

TEST(BotSelectorTest, MentionHandlesPunctuationBoundaries)
{
    EXPECT_TRUE(BotSelector::MentionsName("Thrall! over here", "Thrall"));
    EXPECT_TRUE(BotSelector::MentionsName("(thrall)", "Thrall"));
    EXPECT_TRUE(BotSelector::MentionsName("ok, thrall?", "Thrall"));
}

TEST(BotSelectorTest, ReplyChanceByKindAndSender)
{
    sLlmConfig->playerReplyChanceSay = 90;
    sLlmConfig->botReplyChanceSay = 10;
    sLlmConfig->playerReplyChanceGuild = 70;
    sLlmConfig->botReplyChanceGuild = 5;

    EXPECT_EQ(BotSelector::ReplyChance(TRIGGER_CHAT_SAY, false), 90u);
    EXPECT_EQ(BotSelector::ReplyChance(TRIGGER_CHAT_SAY, true), 10u);
    EXPECT_EQ(BotSelector::ReplyChance(TRIGGER_CHAT_GUILD, false), 70u);
    EXPECT_EQ(BotSelector::ReplyChance(TRIGGER_CHAT_GUILD, true), 5u);
    EXPECT_EQ(BotSelector::ReplyChance(TRIGGER_CHAT_WHISPER, true), 100u);
}
