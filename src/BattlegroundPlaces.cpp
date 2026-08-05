/*
 * This file is part of mod-llm for Felworld. Released under the MIT license
 * (see the LICENSE file at the module root).
 */

#include "BattlegroundPlaces.h"

#include <limits>
#include <span>

namespace ModLlm::BattlegroundPlaces
{
    namespace
    {
        // A named point with a radius, standing in for the region players
        // mean by the name. Coordinates come from the game's own object
        // placements (flag stands, banners, gates, graveyards) and from
        // mod-playerbots' waypoint tables where the core names nothing (the
        // Warsong tunnels, ramps and roofs, Alterac's Field of Strife).
        struct Place
        {
            float x = 0.f;
            float y = 0.f;
            float z = 0.f;
            float radius = 0.f;
            char const* alliance = nullptr; // phrase from an Alliance bot's perspective
            char const* horde = nullptr;    // nullptr: reads the same from both sides
        };

        // Warsong Gulch stacks its geometry: the tunnel runs beneath the
        // ramp, the roof sits over the flag room. The doubled-z metric keeps
        // those apart wherever the tables meet vertical terrain.
        Place const WARSONG_GULCH[] =
        {
            {1540.4f, 1481.3f, 351.8f, 15.f, "in your flag room", "in their flag room"},
            {916.0f, 1434.4f, 345.4f, 15.f, "in their flag room", "in your flag room"},
            {1502.3f, 1457.5f, 347.6f, 12.f, "on the ramp up to your flag room", "on the ramp up to their flag room"},
            {965.0f, 1459.2f, 338.1f, 12.f, "on the ramp up to their flag room", "on the ramp up to your flag room"},
            {1500.6f, 1472.9f, 373.7f, 18.f, "on the roof of your base", "on the roof of their base"},
            {952.7f, 1445.0f, 367.6f, 18.f, "on the roof of their base", "on the roof of your base"},
            {1348.0f, 1461.1f, 323.2f, 25.f, "in your tunnel", "in their tunnel"},
            {1303.4f, 1460.2f, 317.3f, 25.f, "in your tunnel", "in their tunnel"},
            {1124.4f, 1462.3f, 315.9f, 30.f, "in their tunnel", "in your tunnel"},
            {1129.3f, 1461.0f, 315.2f, 30.f, "in their tunnel", "in your tunnel"},
            {1227.4f, 1476.2f, 307.5f, 40.f, "at mid"},
            {1415.3f, 1554.8f, 343.2f, 20.f, "at your graveyard", "at their graveyard"},
            {1029.1f, 1387.5f, 340.8f, 20.f, "at their graveyard", "at your graveyard"},
        };

        Place const ARATHI_BASIN[] =
        {
            {1166.8f, 1200.1f, -56.7f, 50.f, "at the Stables"},
            {977.0f, 1046.6f, -44.8f, 45.f, "at the Blacksmith"},
            {806.2f, 874.3f, -56.0f, 45.f, "at the Farm"},
            {856.1f, 1148.9f, 11.2f, 40.f, "up at the Lumber Mill"},
            {1146.9f, 848.2f, -110.9f, 50.f, "down at the Gold Mine"},
            {1284.6f, 1281.2f, -16.0f, 35.f, "at your team's starting base", "at their team's starting base"},
            {1354.7f, 1270.3f, -11.1f, 35.f, "at your team's starting base", "at their team's starting base"},
            {708.1f, 708.4f, -17.8f, 35.f, "at their team's starting base", "at your team's starting base"},
            {713.7f, 638.4f, -10.6f, 35.f, "at their team's starting base", "at your team's starting base"},
        };

        Place const EYE_OF_THE_STORM[] =
        {
            {2044.3f, 1729.7f, 1190.0f, 35.f, "at Fel Reaver Ruins"},
            {2048.8f, 1393.7f, 1194.5f, 35.f, "at Blood Elf Tower"},
            {2286.6f, 1402.4f, 1197.1f, 35.f, "at Draenei Ruins"},
            {2284.5f, 1731.2f, 1190.0f, 35.f, "at Mage Tower"},
            {2174.8f, 1569.1f, 1160.4f, 35.f, "at mid, by the flag spawn"},
            {2523.8f, 1596.9f, 1270.2f, 40.f, "on your starting platform", "on their starting platform"},
            {2483.9f, 1597.1f, 1244.7f, 25.f, "on the bridge down from your spawn",
                "on the bridge down from their spawn"},
            {2449.2f, 1601.8f, 1201.6f, 25.f, "on the bridge down from your spawn",
                "on the bridge down from their spawn"},
            {1809.1f, 1540.9f, 1267.1f, 40.f, "on their starting platform", "on your starting platform"},
            {1847.0f, 1539.8f, 1243.1f, 25.f, "on the bridge down from their spawn",
                "on the bridge down from your spawn"},
            {1883.2f, 1532.1f, 1202.1f, 25.f, "on the bridge down from their spawn",
                "on the bridge down from your spawn"},
        };

        // Alterac's places carry proper names both sides use, so most
        // entries read the same from either perspective. Graveyards get two
        // points - the flag and the resurrection circle - because the two
        // sit well apart and players call both by the graveyard's name.
        Place const ALTERAC_VALLEY[] =
        {
            {638.6f, -32.4f, 46.1f, 40.f, "at the Stormpike Aid Station"},
            {643.0f, 44.0f, 69.7f, 35.f, "at the Stormpike Aid Station"},
            {669.0f, -294.1f, 30.3f, 40.f, "at Stormpike Graveyard"},
            {676.0f, -374.0f, 30.0f, 35.f, "at Stormpike Graveyard"},
            {77.8f, -404.7f, 46.8f, 40.f, "at Stonehearth Graveyard"},
            {73.4f, -496.4f, 48.7f, 35.f, "at Stonehearth Graveyard"},
            {-202.6f, -112.7f, 78.5f, 45.f, "at Snowfall Graveyard"},
            {-157.4f, 31.2f, 77.0f, 35.f, "at Snowfall Graveyard"},
            {-612.0f, -396.2f, 60.8f, 40.f, "at Iceblood Graveyard"},
            {-531.2f, -405.2f, 49.6f, 35.f, "at Iceblood Graveyard"},
            {-1082.5f, -346.8f, 54.9f, 40.f, "at Frostwolf Graveyard"},
            {-1090.5f, -253.3f, 57.7f, 35.f, "at Frostwolf Graveyard"},
            {-1402.2f, -307.4f, 89.4f, 35.f, "at the Frostwolf Relief Hut"},
            {-1496.1f, -333.3f, 101.1f, 35.f, "at the Frostwolf Relief Hut"},
            {553.8f, -78.7f, 51.9f, 30.f, "at Dun Baldar South Bunker"},
            {674.0f, -143.1f, 63.7f, 30.f, "at Dun Baldar North Bunker"},
            {203.3f, -360.4f, 56.4f, 30.f, "at Icewing Bunker"},
            {-152.4f, -441.8f, 40.4f, 30.f, "at Stonehearth Bunker"},
            {-571.9f, -262.8f, 75.0f, 30.f, "at Iceblood Tower"},
            {-768.9f, -363.7f, 90.9f, 30.f, "at Tower Point"},
            {-1302.9f, -317.0f, 113.9f, 30.f, "at the Frostwolf East Tower"},
            {-1297.5f, -266.8f, 114.2f, 30.f, "at the Frostwolf West Tower"},
            {722.4f, -11.0f, 50.7f, 35.f, "in your keep, at Vanndar", "in Dun Baldar keep, at Vanndar"},
            {-1370.9f, -219.8f, 98.4f, 35.f, "in Frostwolf Keep, at Drek'Thar", "in your keep, at Drek'Thar"},
            {-57.8f, -286.6f, 15.6f, 35.f, "at Stonehearth Outpost"},
            {-545.2f, -165.4f, 57.8f, 35.f, "at Iceblood Garrison"},
            {-260.8f, -329.4f, 6.7f, 70.f, "out on the Field of Strife"},
            {822.5f, -456.8f, 48.6f, 35.f, "inside Irondeep Mine"},
            {966.4f, -446.6f, 56.6f, 35.f, "inside Irondeep Mine"},
            {952.1f, -335.1f, 63.6f, 35.f, "inside Irondeep Mine"},
            {-860.2f, -12.8f, 70.8f, 35.f, "inside Coldtooth Mine"},
            {-827.6f, -147.3f, 62.6f, 35.f, "inside Coldtooth Mine"},
            {-920.2f, -134.6f, 61.5f, 35.f, "inside Coldtooth Mine"},
            {873.0f, -491.3f, 96.5f, 25.f, "in your starting cave", "at the Alliance starting cave"},
            {-1437.7f, -610.1f, 51.2f, 25.f, "at the Horde starting cave", "in your starting cave"},
        };

        Place const ISLE_OF_CONQUEST[] =
        {
            {776.2f, -804.3f, 6.5f, 45.f, "at the Workshop"},
            {726.4f, -360.2f, 17.8f, 50.f, "at the Docks"},
            {807.8f, -1000.1f, 132.4f, 50.f, "up at the Hangar"},
            {251.0f, -1159.3f, 17.2f, 45.f, "at the Quarry"},
            {1269.5f, -400.8f, 37.6f, 45.f, "at the Refinery"},
            {907.1f, -798.9f, 8.3f, 40.f, "at the central crossroads"},
            {299.2f, -784.6f, 48.9f, 45.f, "inside your keep", "inside their keep"},
            {387.9f, -833.4f, 48.7f, 45.f, "inside your keep", "inside their keep"},
            {278.4f, -877.0f, 48.9f, 40.f, "inside your keep", "inside their keep"},
            {1284.8f, -705.7f, 48.9f, 45.f, "inside their keep", "inside your keep"},
            {1166.3f, -762.4f, 48.6f, 45.f, "inside their keep", "inside your keep"},
            {1302.6f, -814.7f, 48.9f, 40.f, "inside their keep", "inside your keep"},
            {413.5f, -834.0f, 48.5f, 30.f, "at your keep's front gate", "at their keep's front gate"},
            {506.8f, -828.6f, 24.3f, 30.f, "at your keep's front gate", "at their keep's front gate"},
            {351.6f, -762.8f, 48.9f, 25.f, "at a side gate of your keep", "at a side gate of their keep"},
            {351.0f, -903.3f, 48.9f, 25.f, "at a side gate of your keep", "at a side gate of their keep"},
            {1150.9f, -762.6f, 47.5f, 30.f, "at their keep's front gate", "at your keep's front gate"},
            {1091.3f, -763.6f, 42.4f, 30.f, "at their keep's front gate", "at your keep's front gate"},
            {1218.7f, -851.2f, 48.3f, 25.f, "at a side gate of their keep", "at a side gate of your keep"},
            {1217.9f, -676.9f, 47.6f, 25.f, "at a side gate of their keep", "at a side gate of your keep"},
        };

        // Strand's sides swap between rounds, so nothing here is phrased
        // your/their - the gates carry colours and the beach is the beach,
        // which is how players call the map anyway.
        Place const STRAND_OF_THE_ANCIENTS[] =
        {
            {1600.4f, -106.3f, 8.9f, 60.f, "down on the beach"},
            {1575.1f, 98.9f, 2.8f, 60.f, "down on the beach"},
            {1575.6f, -158.4f, 5.0f, 45.f, "down on the beach"},
            {1457.2f, -53.7f, 5.2f, 35.f, "at the beach graveyard"},
            {1411.6f, 108.2f, 28.7f, 30.f, "at the Green Emerald gate"},
            {1431.3f, -219.4f, 30.9f, 30.f, "at the Blue Sapphire gate"},
            {1227.7f, -212.6f, 55.4f, 30.f, "at the Red Sun gate"},
            {1214.7f, 81.2f, 53.4f, 30.f, "at the Purple Amethyst gate"},
            {1055.5f, -108.1f, 82.1f, 30.f, "at the Yellow Moon gate"},
            {878.6f, -108.2f, 117.8f, 30.f, "at the Ancient gate"},
            {1309.1f, 9.4f, 30.9f, 35.f, "at the left graveyard"},
            {1388.8f, 203.4f, 32.2f, 35.f, "at the left graveyard"},
            {1338.9f, -153.3f, 30.9f, 35.f, "at the right graveyard"},
            {1396.1f, -288.0f, 32.1f, 35.f, "at the right graveyard"},
            {1215.1f, -65.7f, 70.1f, 30.f, "at the central graveyard"},
            {1122.3f, 4.4f, 68.9f, 30.f, "at the central graveyard"},
            {1043.7f, -88.0f, 87.1f, 35.f, "in the courtyard behind the Yellow Moon gate"},
            {964.6f, -189.8f, 90.7f, 35.f, "at the courtyard graveyard, before the Ancient gate"},
            {837.1f, -107.5f, 127.0f, 30.f, "in the relic chamber"},
        };

        // Nearest place that (x, y, z) is inside of. Vertical distance
        // counts double: the maps stack real places on top of each other
        // (the Warsong tunnel under the ramp, the Lumber Mill over the
        // valley, the Gold Mine underground), and a player twenty yards
        // below a spot is not "at" it.
        Place const* Nearest(std::span<Place const> places, float x, float y, float z)
        {
            Place const* best = nullptr;
            float bestDistSq = std::numeric_limits<float>::max();
            for (Place const& place : places)
            {
                float dx = x - place.x;
                float dy = y - place.y;
                float dz = 2.f * (z - place.z);
                float distSq = dx * dx + dy * dy + dz * dz;
                if (distSq <= place.radius * place.radius && distSq < bestDistSq)
                {
                    best = &place;
                    bestDistSq = distSq;
                }
            }
            return best;
        }
    }

    std::string Locate(BattlegroundTypeId bgType, TeamId perspective, float x, float y, float z)
    {
        std::span<Place const> places;
        switch (bgType)
        {
            case BATTLEGROUND_WS:
                places = WARSONG_GULCH;
                break;
            case BATTLEGROUND_AB:
                places = ARATHI_BASIN;
                break;
            case BATTLEGROUND_EY:
                places = EYE_OF_THE_STORM;
                break;
            case BATTLEGROUND_AV:
                places = ALTERAC_VALLEY;
                break;
            case BATTLEGROUND_IC:
                places = ISLE_OF_CONQUEST;
                break;
            case BATTLEGROUND_SA:
                places = STRAND_OF_THE_ANCIENTS;
                break;
            default:
                return "";
        }

        Place const* place = Nearest(places, x, y, z);
        if (!place)
            return "";
        return perspective == TEAM_HORDE && place->horde ? place->horde : place->alliance;
    }
}
