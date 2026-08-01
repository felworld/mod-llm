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

TEST(BotSelectorTest, NormalizeChatLinksReducesLinksToVisibleText)
{
    EXPECT_EQ(BotSelector::NormalizeChatLinks(
        "anyone for |cffffff00|Hquest:176:8|h[The Family and the Fishing Pole]|h|r ?"),
        "anyone for [The Family and the Fishing Pole] ?");
    EXPECT_EQ(BotSelector::NormalizeChatLinks(
        "|cffffff00|Hquest:1:1|h[One]|h|r and |cffffff00|Hquest:2:2|h[Two]|h|r"),
        "[One] and [Two]");
}

TEST(BotSelectorTest, NormalizeChatLinksKeepsItemIdsAsTags)
{
    // Item links keep their id as the {item:ID} tag convention, so the
    // market tools can resolve what a player linked in an ad.
    EXPECT_EQ(BotSelector::NormalizeChatLinks(
        "wts |cff0070dd|Hitem:2169:0:0:0:0:0:0:0:0|h[Buzzer Blade]|h|r 5g"),
        "wts [Buzzer Blade] {item:2169} 5g");
    EXPECT_EQ(BotSelector::NormalizeChatLinks(
        "|cffffffff|Hitem:2589:0:0:0:0:0:0:0:0|h[Linen Cloth]|h|r and "
        "|cffffffff|Hitem:2592:0:0:0:0:0:0:0:0|h[Wool Cloth]|h|r"),
        "[Linen Cloth] {item:2589} and [Wool Cloth] {item:2592}");
    // Non-item links stay plain visible text.
    EXPECT_EQ(BotSelector::NormalizeChatLinks(
        "|cff71d5ff|Hspell:1459|h[Arcane Intellect]|h|r"),
        "[Arcane Intellect]");
}

TEST(BotSelectorTest, NormalizeChatLinksLeavesPlainTextAlone)
{
    EXPECT_EQ(BotSelector::NormalizeChatLinks("no links here"), "no links here");
    EXPECT_EQ(BotSelector::NormalizeChatLinks(""), "");
    EXPECT_EQ(BotSelector::NormalizeChatLinks("a || b"), "a | b");
}

TEST(BotSelectorTest, NormalizeChatLinksSurvivesTruncatedMarkup)
{
    EXPECT_EQ(BotSelector::NormalizeChatLinks("|cffffff00|Hquest:176"), "");
    EXPECT_EQ(BotSelector::NormalizeChatLinks("|c12"), "");
    EXPECT_EQ(BotSelector::NormalizeChatLinks("trailing |"), "trailing |");
    EXPECT_EQ(BotSelector::NormalizeChatLinks("|x kept"), "|x kept");
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
