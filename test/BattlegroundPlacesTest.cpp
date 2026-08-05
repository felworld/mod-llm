/*
 * This file is part of mod-llm for Felworld. Released under the MIT license
 * (see the LICENSE file at the module root).
 */

#include "BattlegroundPlaces.h"
#include "gtest/gtest.h"

using namespace ModLlm;

// cppcheck-suppress syntaxError
TEST(BattlegroundPlacesTest, PerspectiveFlipsSidedNames)
{
    // The Horde flag room in Warsong Gulch reads "theirs" to an Alliance bot
    // and "yours" to a Horde bot.
    EXPECT_EQ(BattlegroundPlaces::Locate(BATTLEGROUND_WS, TEAM_ALLIANCE, 916.0f, 1434.4f, 345.4f),
        "in their flag room");
    EXPECT_EQ(BattlegroundPlaces::Locate(BATTLEGROUND_WS, TEAM_HORDE, 916.0f, 1434.4f, 345.4f),
        "in your flag room");

    // Same flip inside the Isle of Conquest keeps.
    EXPECT_EQ(BattlegroundPlaces::Locate(BATTLEGROUND_IC, TEAM_HORDE, 1284.8f, -705.7f, 48.9f),
        "inside your keep");
    EXPECT_EQ(BattlegroundPlaces::Locate(BATTLEGROUND_IC, TEAM_ALLIANCE, 1284.8f, -705.7f, 48.9f),
        "inside their keep");
}

TEST(BattlegroundPlacesTest, NeutralNamesReadTheSameFromBothSides)
{
    EXPECT_EQ(BattlegroundPlaces::Locate(BATTLEGROUND_AB, TEAM_ALLIANCE, 1166.8f, 1200.1f, -56.7f),
        "at the Stables");
    EXPECT_EQ(BattlegroundPlaces::Locate(BATTLEGROUND_AB, TEAM_HORDE, 1166.8f, 1200.1f, -56.7f),
        "at the Stables");

    EXPECT_EQ(BattlegroundPlaces::Locate(BATTLEGROUND_EY, TEAM_HORDE, 2174.8f, 1569.1f, 1160.4f),
        "at mid, by the flag spawn");
    EXPECT_EQ(BattlegroundPlaces::Locate(BATTLEGROUND_AV, TEAM_ALLIANCE, -57.8f, -286.6f, 15.6f),
        "at Stonehearth Outpost");
    EXPECT_EQ(BattlegroundPlaces::Locate(BATTLEGROUND_SA, TEAM_ALLIANCE, 837.1f, -107.5f, 127.0f),
        "in the relic chamber");
}

TEST(BattlegroundPlacesTest, VerticalDistanceCountsDouble)
{
    // Inside the Alliance-side tunnel in Warsong Gulch.
    EXPECT_EQ(BattlegroundPlaces::Locate(BATTLEGROUND_WS, TEAM_ALLIANCE, 1303.4f, 1460.2f, 317.3f),
        "in your tunnel");

    // Thirty yards straight up - on the terrain over the tunnel - is not
    // "in the tunnel", and nothing else is close enough either.
    EXPECT_EQ(BattlegroundPlaces::Locate(BATTLEGROUND_WS, TEAM_ALLIANCE, 1303.4f, 1460.2f, 347.3f), "");

    // The Arathi valley floor under the Lumber Mill is not "up at the
    // Lumber Mill".
    EXPECT_EQ(BattlegroundPlaces::Locate(BATTLEGROUND_AB, TEAM_ALLIANCE, 856.1f, 1148.9f, -40.0f), "");
}

TEST(BattlegroundPlacesTest, NearestPlaceWinsWhereRadiiOverlap)
{
    // The Horde keep's front gate sits inside the keep entries' radius too;
    // standing on the gate itself names the gate.
    EXPECT_EQ(BattlegroundPlaces::Locate(BATTLEGROUND_IC, TEAM_HORDE, 1150.9f, -762.6f, 47.5f),
        "at your keep's front gate");
}

TEST(BattlegroundPlacesTest, BetweenPlacesNamesNothing)
{
    // On the road between mid and the Alliance tunnel in Warsong Gulch:
    // outside every radius, so no name rather than a wrong one.
    EXPECT_EQ(BattlegroundPlaces::Locate(BATTLEGROUND_WS, TEAM_ALLIANCE, 1270.0f, 1470.0f, 310.0f), "");
}

TEST(BattlegroundPlacesTest, UnknownMapsNameNothing)
{
    EXPECT_EQ(BattlegroundPlaces::Locate(BATTLEGROUND_NA, TEAM_ALLIANCE, 916.0f, 1434.4f, 345.4f), "");
    EXPECT_EQ(BattlegroundPlaces::Locate(BATTLEGROUND_TYPE_NONE, TEAM_HORDE, 0.f, 0.f, 0.f), "");
}
