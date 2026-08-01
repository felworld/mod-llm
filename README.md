# mod-llm

Agentic LLM-driven playerbots for [Felworld](https://github.com/felworld/azerothcore) — a tech
demo of AI "players" (LLM agents + classical game AI) populating and interacting in an MMO
world. Depends on [mod-playerbots](https://github.com/felworld/mod-playerbots) and an
OpenAI-compatible chat-completions endpoint (the `ac-vllm` compose service by default).

Where its predecessor (mod-ollama-chat) asked the model *"what message do you reply with?"*,
mod-llm presents each situation plus a **tool list**, and the model decides what — if anything —
to do:

| Tool | Effect |
|---|---|
| `say` | Send a chat message; the audience is bound by the trigger (whisper → whisper back, party → party, ...) |
| `emote` | Perform a social emote (`/wave`, `/laugh`, ...) |
| `remember` | Write a short note into the bot's persistent private scratchpad (upsert by slug, optionally tied to the player it concerns) |
| `forget` | Delete one of the bot's notes by slug |
| `get_gear` / `get_inventory` | Read tools: hand the bot's equipped gear or bag contents back to the model (each item with an `{item:ID}` link tag) in a follow-up round |
| `invite_to_party` | Invite the player, via a synthetic client packet so all core validation runs |
| `challenge_duel` | Challenge the player to a duel |
| `bg_strategy` | Relay a Warsong Gulch play call ("inc!!", "fc mid") to the whole team — fans out playerbots' `bg strategy` orders, every bot rolling its own compliance ([details](FEATURES.md#battleground-play-calls)) |
| `conjure_refreshments` / `open_portal` | Mage class services: conjure food or water and walk it over to the asker, or open a portal to a capital city ([details](FEATURES.md#class-services)) |
| `summon_player` | Warlock class service: summon the asker with a real Ritual of Summoning — inviting them to the bot's group first if needed, and recruiting nearby bots to help channel ([details](FEATURES.md#class-services)) |

Returning no tool calls is a valid outcome — most moments deserve no reaction. The tool list
offered with each request is filtered by trigger kind and live game state, and executors
re-validate on the world thread at execution time. Follow-up rounds (capped at two per trigger)
feed tool errors and read-tool results back to the model so it can pick an alternative or talk
about what it looked up. Link tags — `{quest:844}` from the bot's quest log, `{item:12640}` from
a read-tool result — are expanded server-side by the `say` tool into real clickable chat links,
so the model never authors raw client link markup.

The guiding rule for prompt context: anything a player would see on their screen belongs in
it — zone, group roster, guild, the bot's own quest log and memory notes, recent conversation,
speech overheard in earshot, and notable game events seen nearby.

The full behavior reference — routing, pacing, faction rules, and the options behind them — is
in [FEATURES.md](FEATURES.md).

## Triggers

- **[Reactive chat](FEATURES.md#reactive-chat)** — say/yell, whispers, party/raid/battleground,
  guild, and channel messages. Who answers is decided by judgment, not dice: a cheap **router**
  call picks which bots, if any, a message was meant for — so "any mages got water?" reaches the
  mage and idle muttering reaches nobody. Replies are staggered and delivered at human typing
  speed; bots hear each other, so bot conversations happen in front of players.
- **[Emotes](FEATURES.md#emotes)** — emotes aimed at a bot or performed nearby, including
  cross-faction ones.
- **[Game events](FEATURES.md#game-events)** — kills, deaths, level-ups, quest completions,
  duels, achievements, notable loot, loot-roll wins/losses, group joins, out-of-group heals —
  narrated into every nearby bot's overheard transcript whether or not anyone was picked to react.
- **[Initiative](FEATURES.md#initiative)** — an idle scheduler gives each bot periodic
  opportunities to act unprompted.

## Architecture

```
hook (world/map thread)          worker threads                  world thread
┌──────────────────────┐   ┌──────────────────────────┐   ┌──────────────────────────┐
│ BotSelector          │   │ PromptAssembler          │   │ LlmToolOperation.Execute │
│ ContextBuilder ──────┼──▶│ ToolRegistry (schemas)   │──▶│  re-resolve by GUID      │
│  (snapshot, no       │   │ HTTP POST /v1/chat/...   │   │  validate args           │
│   game pointers)     │   │ ToolCallParser           │   │  run tool executors      │
└──────────────────────┘   └──────────────────────────┘   └──────────────────────────┘
```

All game-state effects are marshalled to the world thread through mod-playerbots'
`PlayerbotWorldThreadProcessor`; nothing touches game objects from HTTP workers. Requests are
bounded (`LLM.MaxConcurrentRequests` workers, `LLM.MaxQueueSize` queue) so a stalled LLM server
can never back up into the game.

Adding a tool = one `ToolSpec` (name, JSON schema, trigger mask, executor lambda) in
`src/LlmTools.cpp`. The parser, prompts, and marshalling are tool-agnostic.

## Persistence

Two features persist to the characters DB (schema auto-applied at worldserver startup):
`mod_llm_memory`, the per-bot scratchpad of notes the model writes itself (long-term
continuity), and `mod_llm_history_pair` / `mod_llm_history_room`, recency-aware conversation
transcripts fed into prompts (short-term coherence). Details in
[FEATURES.md](FEATURES.md#persistence).

## Configuration

See `conf/mod_llm.conf.dist` for every option (`LLM.*` namespace): endpoint/model/sampling,
concurrency, reply chances, distances, event chances/cooldowns, initiative pacing, memory caps,
history caps, and prompt templates (including the style-exemplar block that sets the bots'
MMO-player register — positive examples only, since negative examples can raise the very
phrasings they forbid on small models).

In-game administration: `.llm status | enable | disable | reload`.

## Tests

Unit tests (tool-call parsing, schema validation, prompt assembly, mention matching) register
with the core `unit_tests` target; configure the build with `-DBUILD_TESTING=ON`.

`tools/voice_harness.py` iterates on the *voice* without a server: it assembles the same request
the module sends (system prompt + style exemplars from a conf file) for a canned battery of
situations — sycophancy bait, refusal bait, small talk — and prints each bot reply, so prompt
revisions can be A/B-compared against a live LLM endpoint. See its docstring.

## License

MIT (see LICENSE). Bundles nlohmann/json and cpp-httplib, both MIT.

Felworld is a non-commercial research project. It contains no game client,
assets, or proprietary code, and is not affiliated with or endorsed by
Blizzard Entertainment — see the
[project disclaimer](https://github.com/felworld/azerothcore#license-and-disclaimer).
