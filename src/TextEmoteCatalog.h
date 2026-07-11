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
    // Emote name ("wave") -> TEXT_EMOTE_* id, or 0 if unknown.
    uint32 FindId(std::string const& name);

    // TEXT_EMOTE_* id -> emote name, or empty string if not catalogued.
    std::string FindName(uint32 id);

    // All catalogued names, for the emote tool's enum schema.
    std::vector<std::string> const& AllNames();
}

#endif
