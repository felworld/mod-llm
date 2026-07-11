/*
 * This file is part of mod-llm for Felworld. Released under the MIT license
 * (see the LICENSE file at the module root).
 */

#include "TextEmoteCatalog.h"

#include "SharedDefines.h"

#include <algorithm>

namespace ModLlm::TextEmoteCatalog
{
    namespace
    {
        struct Entry
        {
            uint32 id;
            char const* name;
        };

        // Common social emotes only - enough for expressive bots without
        // flooding the tool schema. Ids are TEXT_EMOTE_* from SharedDefines.h.
        Entry const CATALOG[] =
        {
            { TEXT_EMOTE_APPLAUD, "applaud" },
            { TEXT_EMOTE_BOW, "bow" },
            { TEXT_EMOTE_CHEER, "cheer" },
            { TEXT_EMOTE_CHICKEN, "chicken" },
            { TEXT_EMOTE_CONFUSED, "confused" },
            { TEXT_EMOTE_CRY, "cry" },
            { TEXT_EMOTE_CURIOUS, "curious" },
            { TEXT_EMOTE_DANCE, "dance" },
            { TEXT_EMOTE_FLEX, "flex" },
            { TEXT_EMOTE_GREET, "greet" },
            { TEXT_EMOTE_GRIN, "grin" },
            { TEXT_EMOTE_HUG, "hug" },
            { TEXT_EMOTE_KNEEL, "kneel" },
            { TEXT_EMOTE_LAUGH, "laugh" },
            { TEXT_EMOTE_NOD, "nod" },
            { TEXT_EMOTE_POINT, "point" },
            { TEXT_EMOTE_ROAR, "roar" },
            { TEXT_EMOTE_RUDE, "rude" },
            { TEXT_EMOTE_SALUTE, "salute" },
            { TEXT_EMOTE_SHRUG, "shrug" },
            { TEXT_EMOTE_SIGH, "sigh" },
            { TEXT_EMOTE_THANK, "thank" },
            { TEXT_EMOTE_WAVE, "wave" },
            { TEXT_EMOTE_YAWN, "yawn" },
        };
    }

    uint32 FindId(std::string const& name)
    {
        std::string lowered = name;
        std::transform(lowered.begin(), lowered.end(), lowered.begin(),
            [](unsigned char c) { return std::tolower(c); });

        for (Entry const& entry : CATALOG)
            if (lowered == entry.name)
                return entry.id;

        return 0;
    }

    std::string FindName(uint32 id)
    {
        for (Entry const& entry : CATALOG)
            if (entry.id == id)
                return entry.name;

        return "";
    }

    std::vector<std::string> const& AllNames()
    {
        static std::vector<std::string> const names = []
        {
            std::vector<std::string> result;
            for (Entry const& entry : CATALOG)
                result.emplace_back(entry.name);
            return result;
        }();

        return names;
    }
}
