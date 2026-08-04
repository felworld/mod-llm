/*
 * This file is part of mod-llm for Felworld. Released under the MIT license
 * (see the LICENSE file at the module root).
 */

#include "GuildFlavor.h"
#include "gtest/gtest.h"

using namespace ModLlm;

namespace
{
    FlavorProfile Profile(std::string const& text)
    {
        FlavorProfile profile;
        EXPECT_TRUE(GuildFlavors::Deserialize(text, profile)) << text;
        return profile;
    }
}

// cppcheck-suppress syntaxError
TEST(GuildFlavorTest, SerializationRoundTrips)
{
    FlavorProfile profile;
    ASSERT_TRUE(GuildFlavors::Deserialize("rp+wpvp", profile));
    ASSERT_EQ(profile.size(), 2u);
    EXPECT_EQ(profile[0], GuildFlavor::Rp);
    EXPECT_EQ(profile[1], GuildFlavor::Wpvp);
    EXPECT_EQ(GuildFlavors::Serialize(profile), "rp+wpvp");

    // Order is the identity: the same tags the other way round are a
    // different profile and serialize differently.
    ASSERT_TRUE(GuildFlavors::Deserialize("wpvp+rp", profile));
    EXPECT_EQ(GuildFlavors::Serialize(profile), "wpvp+rp");

    // Whitespace and case are tolerated on the way in, never on the way out.
    ASSERT_TRUE(GuildFlavors::Deserialize(" Social + Leveling ", profile));
    EXPECT_EQ(GuildFlavors::Serialize(profile), "social+leveling");
}

TEST(GuildFlavorTest, DeserializeRejectsBadInput)
{
    FlavorProfile profile;
    EXPECT_FALSE(GuildFlavors::Deserialize("", profile));
    EXPECT_FALSE(GuildFlavors::Deserialize("questing", profile));
    EXPECT_FALSE(GuildFlavors::Deserialize("rp+rp", profile));
    EXPECT_FALSE(GuildFlavors::Deserialize("rp+", profile));
    EXPECT_FALSE(GuildFlavors::Deserialize("+rp", profile));
}

TEST(GuildFlavorTest, ParseProfilesSkipsBadEntries)
{
    std::vector<WeightedProfile> profiles = GuildFlavors::ParseProfiles(
        "social+leveling:25, questing:10, raiding:0, pvp+pvp:5, rp, wpvp:eight, raiding:12");

    ASSERT_EQ(profiles.size(), 2u);
    EXPECT_EQ(GuildFlavors::Serialize(profiles[0].profile), "social+leveling");
    EXPECT_EQ(profiles[0].weight, 25u);
    EXPECT_EQ(GuildFlavors::Serialize(profiles[1].profile), "raiding");
    EXPECT_EQ(profiles[1].weight, 12u);
    EXPECT_EQ(GuildFlavors::TotalWeight(profiles), 37u);

    EXPECT_TRUE(GuildFlavors::ParseProfiles("").empty());
    EXPECT_EQ(GuildFlavors::TotalWeight({}), 0u);
}

TEST(GuildFlavorTest, ParseProfilesReadsTheShippedDefault)
{
    std::vector<WeightedProfile> profiles = GuildFlavors::ParseProfiles(
        "social+leveling:25, leveling:20, rp+leveling:10, pvp+leveling:10, wpvp+leveling:8, "
        "rp+wpvp:8, raiding+pvp:7, raiding:12");

    EXPECT_EQ(profiles.size(), 8u);
    EXPECT_EQ(GuildFlavors::TotalWeight(profiles), 100u);
}

TEST(GuildFlavorTest, PickIsWeightedAndDeterministic)
{
    std::vector<WeightedProfile> profiles = GuildFlavors::ParseProfiles("social:2, raiding:3");

    // Roll 0..1 lands on the first entry, 2..4 on the second, and the roll
    // wraps so any random source works.
    EXPECT_EQ(GuildFlavors::Serialize(*GuildFlavors::Pick(profiles, 0)), "social");
    EXPECT_EQ(GuildFlavors::Serialize(*GuildFlavors::Pick(profiles, 1)), "social");
    EXPECT_EQ(GuildFlavors::Serialize(*GuildFlavors::Pick(profiles, 2)), "raiding");
    EXPECT_EQ(GuildFlavors::Serialize(*GuildFlavors::Pick(profiles, 4)), "raiding");
    EXPECT_EQ(GuildFlavors::Serialize(*GuildFlavors::Pick(profiles, 5)), "social");

    EXPECT_EQ(GuildFlavors::Pick({}, 0), nullptr);
}

TEST(GuildFlavorTest, IdentityClauseLeadsWithThePrimaryTag)
{
    EXPECT_EQ(GuildFlavors::IdentityClause(Profile("raiding")), "a raiding guild");
    EXPECT_EQ(GuildFlavors::IdentityClause(Profile("rp+wpvp")),
        "a roleplay guild that also fights in world PvP");
    EXPECT_EQ(GuildFlavors::IdentityClause(Profile("social+leveling")),
        "a casual social guild that also levels together");
    EXPECT_EQ(GuildFlavors::IdentityClause(Profile("wpvp+rp")),
        "a world PvP guild that also keeps to character");
    EXPECT_EQ(GuildFlavors::IdentityClause({}), "");

    // Three tags chain rather than repeating "that also".
    FlavorProfile three = { GuildFlavor::Raiding, GuildFlavor::Pvp, GuildFlavor::Social };
    EXPECT_EQ(GuildFlavors::IdentityClause(three),
        "a raiding guild that also runs battlegrounds together and is here for the company");
}

TEST(GuildFlavorTest, GuidanceComposesFromEveryTagPresent)
{
    std::string chat = GuildFlavors::ChatGuidance(Profile("wpvp+rp"));
    EXPECT_NE(chat.find("enemy sightings"), std::string::npos);
    // The in-character rule applies wherever rp sits in the profile.
    EXPECT_NE(chat.find("in character"), std::string::npos);
    EXPECT_EQ(GuildFlavors::ChatGuidance({}), "");

    std::string recruit = GuildFlavors::RecruitGuidance(Profile("raiding+pvp"));
    EXPECT_NE(recruit.find("raid content"), std::string::npos);
    EXPECT_NE(recruit.find("battlegrounds"), std::string::npos);
    // Raiding sells ambition, never a schedule the bots do not keep.
    EXPECT_EQ(recruit.find("every"), std::string::npos);

    std::string invite = GuildFlavors::InviteGuidance(Profile("rp+wpvp"));
    EXPECT_NE(invite.find("a roleplay guild that also fights in world PvP"), std::string::npos);
    EXPECT_NE(invite.find("guild_invite"), std::string::npos);
    EXPECT_EQ(GuildFlavors::InviteGuidance({}), "");

    EXPECT_EQ(GuildFlavors::FlavorLine(Profile("raiding")), "flavor: a raiding guild");
    EXPECT_EQ(GuildFlavors::FlavorLine({}), "");
}

TEST(GuildFlavorTest, MotdIsStampedFromThePrimaryTag)
{
    std::string first = GuildFlavors::MotdFor(Profile("social+leveling"), "Dawnbreakers", 0);
    std::string second = GuildFlavors::MotdFor(Profile("social+leveling"), "Dawnbreakers", 1);
    EXPECT_FALSE(first.empty());
    EXPECT_NE(first, second);
    // Variants wrap, so any roll is a valid variant.
    EXPECT_EQ(GuildFlavors::MotdFor(Profile("social+leveling"), "Dawnbreakers", 2), first);

    // The name is substituted where the template asks for it, and the
    // secondary tag adds its clause.
    EXPECT_NE(second.find("Dawnbreakers"), std::string::npos);
    EXPECT_NE(second.find("Leveling runs"), std::string::npos);
    EXPECT_EQ(second.find("{guild}"), std::string::npos);

    // rp+wpvp has bespoke copy rather than a composed clause.
    std::string bespoke = GuildFlavors::MotdFor(Profile("rp+wpvp"), "Ashen Vow", 0);
    EXPECT_NE(bespoke.find("Ashen Vow"), std::string::npos);
    EXPECT_EQ(bespoke.find("World PvP calls"), std::string::npos);

    EXPECT_EQ(GuildFlavors::MotdFor({}, "Dawnbreakers", 0), "");
}

TEST(GuildFlavorTest, ColdPitchSkipsLowbiesWithoutTheLevelingTag)
{
    // A leveling guild has something for anybody.
    EXPECT_TRUE(GuildFlavors::WouldColdPitch(Profile("wpvp+leveling"), 12, 80));

    // An endgame guild waits for someone within reach of the cap.
    EXPECT_FALSE(GuildFlavors::WouldColdPitch(Profile("raiding+pvp"), 12, 80));
    EXPECT_FALSE(GuildFlavors::WouldColdPitch(Profile("raiding+pvp"), 55, 80));
    EXPECT_TRUE(GuildFlavors::WouldColdPitch(Profile("raiding+pvp"), 56, 80));

    // The threshold follows the realm's level cap.
    EXPECT_TRUE(GuildFlavors::WouldColdPitch(Profile("raiding"), 42, 60));
    EXPECT_FALSE(GuildFlavors::WouldColdPitch(Profile("raiding"), 41, 60));

    // An unflavored guild pitches whoever it meets, as it always did.
    EXPECT_TRUE(GuildFlavors::WouldColdPitch({}, 5, 80));
}

TEST(GuildFlavorTest, TagNamesRoundTripThroughParse)
{
    for (uint8 i = 0; i <= uint8(GuildFlavor::Social); ++i)
    {
        GuildFlavor flavor = GuildFlavor(i);
        GuildFlavor parsed = GuildFlavor::Social;
        ASSERT_TRUE(GuildFlavors::ParseTag(GuildFlavors::Name(flavor), parsed));
        EXPECT_EQ(parsed, flavor);
    }

    GuildFlavor ignored = GuildFlavor::Social;
    EXPECT_FALSE(GuildFlavors::ParseTag("pve", ignored));
}
