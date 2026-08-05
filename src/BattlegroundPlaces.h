/*
 * This file is part of mod-llm for Felworld. Released under the MIT license
 * (see the LICENSE file at the module root).
 */

#ifndef MOD_LLM_BATTLEGROUND_PLACES_H
#define MOD_LLM_BATTLEGROUND_PLACES_H

#include "SharedDefines.h"

#include <string>

namespace ModLlm
{
    // Where inside a battleground a set of coordinates is, phrased the way a
    // player standing there would name the spot: "in the tunnel", "at the
    // Blacksmith", "out on the Field of Strife". The area name a bot already
    // has is too coarse for a callout - half of Warsong Gulch shares one
    // area - and the place is the part of a callout only the one standing
    // there can supply.
    namespace BattlegroundPlaces
    {
        // The named place at (x, y, z) on `bgType`'s map, with a leading
        // preposition so it drops into a sentence ("you are ..."), from
        // `perspective`'s side of the map ("your flag room" vs "their flag
        // room"). Empty when nowhere nameable is close enough - the road
        // between spots on the larger maps.
        std::string Locate(BattlegroundTypeId bgType, TeamId perspective, float x, float y, float z);
    }
}

#endif
