/*
 * This file is part of mod-llm for Felworld. Released under the MIT license
 * (see the LICENSE file at the module root).
 */

#include "TextEmoteCatalog.h"

#include "StringFormat.h"

#include <algorithm>

namespace ModLlm::TextEmoteCatalog
{
    namespace
    {
        struct Entry
        {
            uint32 id;
            char const* name;
            // Untargeted / at-you / at-named, male then female actor;
            // nullptr falls back female -> male, targeted -> untargeted.
            char const* phrases[6];
        };

        // Every emote the client can send, with the third-person lines
        // other players see.
        Entry const CATALOG[] =
        {
#include "TextEmotePhrases.inc"
        };

        // The subset offered in the emote tool's schema: the social and
        // player-culture staples a bot might plausibly reach for.
        char const* const OUTGOING[] =
        {
            "applaud", "beg", "bow", "bye", "cheer", "chicken", "confused",
            "congratulate", "cower", "cry", "dance", "facepalm", "flex",
            "flirt", "golfclap", "greet", "grin", "hug", "joke", "kiss",
            "laugh", "mock", "moo", "mourn", "no", "nod", "pat", "point",
            "poke", "rasp", "roar", "rofl", "rolleyes", "rude", "salute",
            "shakefist", "shrug", "sigh", "sleep", "spit", "taunt", "thank",
            "threaten", "train", "violin", "wave", "wink", "yawn",
        };

        Entry const* Find(uint32 id)
        {
            for (Entry const& entry : CATALOG)
                if (entry.id == id)
                    return &entry;

            return nullptr;
        }
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

    std::vector<std::string> const& AllNames()
    {
        static std::vector<std::string> const names(std::begin(OUTGOING), std::end(OUTGOING));
        return names;
    }

    std::string Describe(uint32 id, Target target, bool female, std::string const& targetName)
    {
        Entry const* entry = Find(id);
        if (!entry)
            return "";

        auto pick = [&](size_t slot) -> char const*
        {
            if (female && entry->phrases[slot + 3])
                return entry->phrases[slot + 3];
            return entry->phrases[slot];
        };

        if (target == Target::Named)
            if (char const* phrase = pick(2))
                return Acore::StringFormat(phrase, targetName);

        char const* phrase = target == Target::You ? pick(1) : nullptr;
        if (!phrase)
            phrase = pick(0);

        return phrase ? phrase : "";
    }
}
