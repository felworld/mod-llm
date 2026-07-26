/*
 * This file is part of mod-llm for Felworld. Released under the MIT license
 * (see the LICENSE file at the module root).
 */

#ifndef MOD_LLM_TEXT_EMOTE_CATALOG_H
#define MOD_LLM_TEXT_EMOTE_CATALOG_H

#include "Define.h"

#include <string>
#include <vector>

namespace ModLlm::TextEmoteCatalog
{
    // Who an incoming emote was aimed at, from the reacting bot's viewpoint.
    enum class Target
    {
        None,
        You,
        Named,
    };

    // Emote name ("wave") -> TEXT_EMOTE_* id, or 0 if unknown. Resolves
    // against every client emote, not just the curated tool list, so a
    // model that picks an off-list name still lands on a real emote.
    uint32 FindId(std::string const& name);

    // Curated names for the emote tool's enum schema: broad enough for
    // expressive bots without drowning the schema in 250 entries.
    std::vector<std::string> const& AllNames();

    // The third-person phrase other players see for an incoming emote
    // ("makes a rude gesture at you"), straight from the client's emote
    // text data. Empty when the emote has no text at all (animation or
    // sound only): the right reaction to those is silence, not a guess.
    std::string Describe(uint32 id, Target target, bool female, std::string const& targetName = "");
}

#endif
