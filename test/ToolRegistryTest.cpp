/*
 * This file is part of mod-llm for Felworld. Released under the MIT license
 * (see the LICENSE file at the module root).
 */

#include "ToolRegistry.h"
#include "gtest/gtest.h"

using namespace ModLlm;

namespace
{
    nlohmann::json TestSchema()
    {
        return {
            { "type", "object" },
            { "properties", {
                { "message", { { "type", "string" } } },
                { "count", { { "type", "integer" } } },
                { "direction", { { "type", "string" }, { "enum", { "up", "down" } } } }
            } },
            { "required", { "message" } }
        };
    }
}

// cppcheck-suppress syntaxError
TEST(ToolRegistryTest, BuildToolsArrayFiltersByTriggerMask)
{
    ToolRegistry registry;
    registry.Register({ "chat_only", "", { { "type", "object" } }, TRIGGER_CHAT_WHISPER, false, nullptr });
    registry.Register({ "everywhere", "", { { "type", "object" } }, TRIGGER_ALL, false, nullptr });

    nlohmann::json whisperTools = registry.BuildToolsArray(TRIGGER_CHAT_WHISPER);
    EXPECT_EQ(whisperTools.size(), 2u);

    nlohmann::json initiativeTools = registry.BuildToolsArray(TRIGGER_INITIATIVE);
    ASSERT_EQ(initiativeTools.size(), 1u);
    EXPECT_EQ(initiativeTools[0]["function"]["name"], "everywhere");
}

TEST(ToolRegistryTest, BuildToolsArrayOmitsActorToolsWithoutActor)
{
    ToolRegistry registry;
    registry.Register({ "needs_actor", "", { { "type", "object" } }, TRIGGER_ALL, true, nullptr });
    registry.Register({ "standalone", "", { { "type", "object" } }, TRIGGER_ALL, false, nullptr });

    nlohmann::json tools = registry.BuildToolsArray(TRIGGER_ALL, nullptr, nullptr);
    ASSERT_EQ(tools.size(), 1u);
    EXPECT_EQ(tools[0]["function"]["name"], "standalone");
}

TEST(ToolRegistryTest, BuildToolsArrayHonorsAvailabilityPredicate)
{
    ToolRegistry registry;
    registry.Register({ "hidden", "", { { "type", "object" } }, TRIGGER_ALL, false, nullptr,
        [](Player*, Player*) { return false; } });
    registry.Register({ "offered", "", { { "type", "object" } }, TRIGGER_ALL, false, nullptr,
        [](Player*, Player*) { return true; } });

    nlohmann::json tools = registry.BuildToolsArray(TRIGGER_ALL, nullptr, nullptr);
    ASSERT_EQ(tools.size(), 1u);
    EXPECT_EQ(tools[0]["function"]["name"], "offered");
}

TEST(ToolRegistryTest, FindLocatesRegisteredTool)
{
    ToolRegistry registry;
    registry.Register({ "say", "", { { "type", "object" } }, TRIGGER_ALL, false, nullptr });

    EXPECT_NE(registry.Find("say"), nullptr);
    EXPECT_EQ(registry.Find("unknown_tool"), nullptr);
}

TEST(ToolRegistryTest, ValidateAcceptsGoodArgs)
{
    std::string error;
    nlohmann::json args = { { "message", "hello" }, { "count", 3 }, { "direction", "up" } };
    EXPECT_TRUE(ToolRegistry::ValidateArgs(TestSchema(), args, error)) << error;
}

TEST(ToolRegistryTest, ValidateRejectsNonObject)
{
    std::string error;
    EXPECT_FALSE(ToolRegistry::ValidateArgs(TestSchema(), nlohmann::json::array(), error));
    EXPECT_FALSE(ToolRegistry::ValidateArgs(TestSchema(), nlohmann::json("string"), error));
}

TEST(ToolRegistryTest, ValidateRejectsMissingRequired)
{
    std::string error;
    nlohmann::json args = { { "count", 3 } };
    EXPECT_FALSE(ToolRegistry::ValidateArgs(TestSchema(), args, error));
    EXPECT_NE(error.find("message"), std::string::npos);
}

TEST(ToolRegistryTest, ValidateRejectsUndeclaredKey)
{
    std::string error;
    nlohmann::json args = { { "message", "hi" }, { "bogus", 1 } };
    EXPECT_FALSE(ToolRegistry::ValidateArgs(TestSchema(), args, error));
}

TEST(ToolRegistryTest, ValidateRejectsWrongType)
{
    std::string error;
    nlohmann::json args = { { "message", 42 } };
    EXPECT_FALSE(ToolRegistry::ValidateArgs(TestSchema(), args, error));

    args = { { "message", "ok" }, { "count", "three" } };
    EXPECT_FALSE(ToolRegistry::ValidateArgs(TestSchema(), args, error));
}

TEST(ToolRegistryTest, ValidateRejectsEnumViolation)
{
    std::string error;
    nlohmann::json args = { { "message", "ok" }, { "direction", "sideways" } };
    EXPECT_FALSE(ToolRegistry::ValidateArgs(TestSchema(), args, error));
}
