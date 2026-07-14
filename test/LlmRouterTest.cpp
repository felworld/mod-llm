/*
 * This file is part of mod-llm for Felworld. Released under the MIT license
 * (see the LICENSE file at the module root).
 */

#include "LlmRouter.h"
#include "gtest/gtest.h"

using namespace ModLlm;

// cppcheck-suppress syntaxError
TEST(LlmRouterTest, ParsesBareArray)
{
    auto names = Router::ParseRouterReply(R"(["Zujin", "Mera"])");
    ASSERT_TRUE(names.has_value());
    ASSERT_EQ(names->size(), 2u);
    EXPECT_EQ((*names)[0], "Zujin");
    EXPECT_EQ((*names)[1], "Mera");
}

TEST(LlmRouterTest, ParsesArrayWrappedInProse)
{
    auto names = Router::ParseRouterReply("The mage should answer: [\"Mera\"] since they asked for water.");
    ASSERT_TRUE(names.has_value());
    ASSERT_EQ(names->size(), 1u);
    EXPECT_EQ((*names)[0], "Mera");
}

TEST(LlmRouterTest, SkipsNonJsonBracketPairs)
{
    auto names = Router::ParseRouterReply("[thinking] water is a mage thing, so [\"Mera\"]");
    ASSERT_TRUE(names.has_value());
    ASSERT_EQ(names->size(), 1u);
    EXPECT_EQ((*names)[0], "Mera");
}

TEST(LlmRouterTest, EmptyArrayMeansNobody)
{
    auto names = Router::ParseRouterReply("[]");
    ASSERT_TRUE(names.has_value());
    EXPECT_TRUE(names->empty());
}

TEST(LlmRouterTest, NoArrayIsNotParseable)
{
    EXPECT_FALSE(Router::ParseRouterReply("nobody needs to answer this").has_value());
    EXPECT_FALSE(Router::ParseRouterReply("").has_value());
    EXPECT_FALSE(Router::ParseRouterReply("[unclosed").has_value());
}

TEST(LlmRouterTest, SkipsNonStringEntries)
{
    auto names = Router::ParseRouterReply(R"(["Zujin", 3, {"name": "Mera"}])");
    ASSERT_TRUE(names.has_value());
    ASSERT_EQ(names->size(), 1u);
    EXPECT_EQ((*names)[0], "Zujin");
}
