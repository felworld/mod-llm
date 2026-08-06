/*
 * This file is part of mod-llm for Felworld. Released under the MIT license
 * (see the LICENSE file at the module root).
 */

#include "LlmTools.h"
#include "gtest/gtest.h"

using namespace ModLlm;

namespace
{
    // Stand-in for the game resolver: knows item 42 and quest 7, nothing else.
    std::string FakeResolve(std::string const& kind, uint32 id)
    {
        if (kind == "item" && id == 42)
            return "|cff1eff00|Hitem:42:0:0:0:0:0:0:0|h[Test Blade]|h|r";
        if (kind == "quest" && id == 7)
            return "|cFFFFFF00|Hquest:7:10|h[Test Quest]|h|r";
        return "";
    }
}

// cppcheck-suppress syntaxError
TEST(LlmToolsTest, ExpandChatLinksReplacesKnownTags)
{
    EXPECT_EQ(LlmTools::ExpandChatLinks("check out my {item:42}!", FakeResolve),
        "check out my |cff1eff00|Hitem:42:0:0:0:0:0:0:0|h[Test Blade]|h|r!");
    EXPECT_EQ(LlmTools::ExpandChatLinks("{quest:7} is almost done", FakeResolve),
        "|cFFFFFF00|Hquest:7:10|h[Test Quest]|h|r is almost done");
}

TEST(LlmToolsTest, ExpandChatLinksHandlesMultipleTags)
{
    EXPECT_EQ(LlmTools::ExpandChatLinks("{item:42} and {quest:7}", FakeResolve),
        "|cff1eff00|Hitem:42:0:0:0:0:0:0:0|h[Test Blade]|h|r and |cFFFFFF00|Hquest:7:10|h[Test Quest]|h|r");
}

TEST(LlmToolsTest, ExpandChatLinksDropsUnresolvableTags)
{
    EXPECT_EQ(LlmTools::ExpandChatLinks("I found {item:99999} today", FakeResolve),
        "I found  today");
    EXPECT_EQ(LlmTools::ExpandChatLinks("{spell:1}", FakeResolve), "");
}

TEST(LlmToolsTest, ExpandChatLinksIgnoresKindCase)
{
    EXPECT_EQ(LlmTools::ExpandChatLinks("my {Item:42}", FakeResolve),
        "my |cff1eff00|Hitem:42:0:0:0:0:0:0:0|h[Test Blade]|h|r");
}

TEST(LlmToolsTest, ExpandChatLinksLeavesNonTagBracesAlone)
{
    EXPECT_EQ(LlmTools::ExpandChatLinks("braces {like this} stay", FakeResolve),
        "braces {like this} stay");
    EXPECT_EQ(LlmTools::ExpandChatLinks("{item:notanumber}", FakeResolve), "{item:notanumber}");
    EXPECT_EQ(LlmTools::ExpandChatLinks("{gold:42}", FakeResolve), "{gold:42}");
    EXPECT_EQ(LlmTools::ExpandChatLinks("{item:}", FakeResolve), "{item:}");
    EXPECT_EQ(LlmTools::ExpandChatLinks("unclosed {item:42", FakeResolve), "unclosed {item:42");
    EXPECT_EQ(LlmTools::ExpandChatLinks("no tags at all", FakeResolve), "no tags at all");
}

TEST(LlmToolsTest, ExpandChatLinksHandlesAdjacentAndNestedBraces)
{
    EXPECT_EQ(LlmTools::ExpandChatLinks("{{item:42}}", FakeResolve),
        "{|cff1eff00|Hitem:42:0:0:0:0:0:0:0|h[Test Blade]|h|r}");
    EXPECT_EQ(LlmTools::ExpandChatLinks("{item:42}{quest:7}", FakeResolve),
        "|cff1eff00|Hitem:42:0:0:0:0:0:0:0|h[Test Blade]|h|r|cFFFFFF00|Hquest:7:10|h[Test Quest]|h|r");
}

TEST(LlmToolsTest, ExpandChatLinksRejectsOverlongIds)
{
    EXPECT_EQ(LlmTools::ExpandChatLinks("{item:1234567890123}", FakeResolve), "{item:1234567890123}");
}

TEST(LlmToolsTest, SanitizeChatTextTrimsQuotesAndToolSyntax)
{
    EXPECT_EQ(LlmTools::SanitizeChatText("\"hello there\""), "hello there");
    EXPECT_EQ(LlmTools::SanitizeChatText("  spaced out  "), "spaced out");
    EXPECT_EQ(LlmTools::SanitizeChatText("fine so far<tool_call>{\"name\":\"say\"}"), "fine so far");
    EXPECT_EQ(LlmTools::SanitizeChatText("\" padded inside \""), "padded inside");
}

TEST(LlmToolsTest, SanitizeChatTextFoldsNewlinesIntoOneLine)
{
    EXPECT_EQ(LlmTools::SanitizeChatText("first line\nsecond line"), "first line second line");
    EXPECT_EQ(LlmTools::SanitizeChatText("windows\r\nstyle"), "windows style");
    EXPECT_EQ(LlmTools::SanitizeChatText("a paragraph\n\n\tand another"), "a paragraph and another");
    EXPECT_EQ(LlmTools::SanitizeChatText("\n\nleading and trailing\n\n"), "leading and trailing");
    EXPECT_EQ(LlmTools::SanitizeChatText("\n"), "");
}
