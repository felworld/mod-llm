/*
 * This file is part of mod-llm for Felworld. Released under the MIT license
 * (see the LICENSE file at the module root).
 */

#include "ToolCallParser.h"
#include "gtest/gtest.h"

using namespace ModLlm;

// cppcheck-suppress syntaxError
TEST(ToolCallParserTest, ParsesSingleToolCall)
{
    LlmResponse response = ToolCallParser::Parse(R"({
        "choices": [{ "message": {
            "content": null,
            "tool_calls": [{ "type": "function", "function": {
                "name": "say", "arguments": "{\"message\": \"hi there\"}" } }]
        }, "finish_reason": "tool_calls" }]
    })");

    ASSERT_TRUE(response.ok);
    ASSERT_EQ(response.toolCalls.size(), 1u);
    EXPECT_EQ(response.toolCalls[0].name, "say");
    EXPECT_EQ(response.toolCalls[0].arguments, "{\"message\": \"hi there\"}");
    EXPECT_TRUE(response.content.empty());
}

TEST(ToolCallParserTest, ParsesMultipleToolCalls)
{
    LlmResponse response = ToolCallParser::Parse(R"({
        "choices": [{ "message": { "tool_calls": [
            { "function": { "name": "say", "arguments": "{\"message\": \"gz!\"}" } },
            { "function": { "name": "emote", "arguments": "{\"emote\": \"cheer\"}" } }
        ] } }]
    })");

    ASSERT_TRUE(response.ok);
    ASSERT_EQ(response.toolCalls.size(), 2u);
    EXPECT_EQ(response.toolCalls[1].name, "emote");
}

TEST(ToolCallParserTest, ToleratesInlineArgumentsObject)
{
    LlmResponse response = ToolCallParser::Parse(R"({
        "choices": [{ "message": { "tool_calls": [
            { "function": { "name": "say", "arguments": { "message": "hello" } } }
        ] } }]
    })");

    ASSERT_TRUE(response.ok);
    ASSERT_EQ(response.toolCalls.size(), 1u);
    EXPECT_NE(response.toolCalls[0].arguments.find("hello"), std::string::npos);
}

TEST(ToolCallParserTest, BareContentWithoutToolCalls)
{
    LlmResponse response = ToolCallParser::Parse(R"({
        "choices": [{ "message": { "content": "just words" }, "finish_reason": "stop" }]
    })");

    ASSERT_TRUE(response.ok);
    EXPECT_TRUE(response.toolCalls.empty());
    EXPECT_EQ(response.content, "just words");
}

TEST(ToolCallParserTest, EmptyMessageIsValidNoOp)
{
    LlmResponse response = ToolCallParser::Parse(R"({
        "choices": [{ "message": { "content": null } }]
    })");

    ASSERT_TRUE(response.ok);
    EXPECT_TRUE(response.toolCalls.empty());
    EXPECT_TRUE(response.content.empty());
}

TEST(ToolCallParserTest, MalformedJsonFails)
{
    LlmResponse response = ToolCallParser::Parse("not json at all {");
    EXPECT_FALSE(response.ok);
    EXPECT_FALSE(response.error.empty());
}

TEST(ToolCallParserTest, MissingChoicesFails)
{
    LlmResponse response = ToolCallParser::Parse(R"({ "object": "chat.completion" })");
    EXPECT_FALSE(response.ok);
}

TEST(ToolCallParserTest, ServerErrorIsReported)
{
    LlmResponse response = ToolCallParser::Parse(R"({
        "error": { "message": "model not found", "type": "invalid_request_error" }
    })");

    EXPECT_FALSE(response.ok);
    EXPECT_EQ(response.error, "model not found");
}

TEST(ToolCallParserTest, SkipsToolCallsWithoutName)
{
    LlmResponse response = ToolCallParser::Parse(R"({
        "choices": [{ "message": { "tool_calls": [
            { "function": { "arguments": "{}" } },
            { "function": { "name": "say", "arguments": "{}" } }
        ] } }]
    })");

    ASSERT_TRUE(response.ok);
    ASSERT_EQ(response.toolCalls.size(), 1u);
    EXPECT_EQ(response.toolCalls[0].name, "say");
}
