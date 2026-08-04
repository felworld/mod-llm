/*
 * This file is part of mod-llm for Felworld. Released under the MIT license
 * (see the LICENSE file at the module root).
 */

#ifndef MOD_LLM_BATTLEGROUND_CONTEXT_H
#define MOD_LLM_BATTLEGROUND_CONTEXT_H

#include <string>

class Player;

namespace ModLlm
{
    // What a battleground's HUD tells the player holding it: the score, the
    // objectives and who owns them, the clock. Every fact here is one the
    // client is sent as a world state (or reads out of the match's own
    // announcements), so a bot knows exactly what a player at the keyboard
    // knows - no more. World thread only.
    namespace BattlegroundContext
    {
        // The bot's own scoreboard, as a sentence or two appended to reply
        // guidance, plus the shorthand teammates call plays in. Empty unless
        // the bot is in a battleground (arenas and the post-match wrap-up get
        // nothing).
        std::string Describe(Player* bot);
    }
}

#endif
