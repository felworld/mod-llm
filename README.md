# mod-llm

Agentic LLM-driven playerbots for [Felworld](https://github.com/felworld). Part of the Felworld
project; depends on [mod-playerbots](https://github.com/felworld/mod-playerbots) and an
OpenAI-compatible chat-completions endpoint (the `ac-vllm` compose service by default).

Where its predecessor (mod-ollama-chat) asked the model *"what message do you reply with?"*,
mod-llm presents each situation plus a **tool list**, and the model decides what — if anything —
to do:

| Tool | Effect |
|---|---|
| `say` | Send a chat message; the audience is bound by the trigger (whisper → whisper back, party → party, ...) |
| `emote` | Perform a social emote (`/wave`, `/laugh`, ...) |
| `adjust_sentiment` | Nudge the bot's persistent 0..1 opinion of the player up or down |
| `invite_to_party` | Invite the player, via a synthetic client packet so all core validation runs |
| `challenge_duel` | Challenge the player to a duel |

Returning no tool calls is a valid outcome — most moments deserve no reaction. The tool list
offered with each request is filtered by trigger kind *and* by live game state — a bot is never
offered `invite_to_party` for someone already in its group (or otherwise uninvitable), nor
`challenge_duel` when a duel can't start. Executors still re-validate at execution time, since
state can change while the request is in flight; if a call does fail, the error is fed back to
the model once — as OpenAI tool-result messages, with context and tool list rebuilt against the
new game state — so it can pick an alternative action (`LLM.ErrorFeedback.Enable`).

The guiding rule for prompt context: anything a player would see on their screen belongs in it.
Today that's the bot's zone, its full party/raid roster (names and leader), guild, its lasting
sentiment toward the other player, recent conversation history, and everything said in /say or
/yell within earshot lately — bots overhear like players do, whether or not they were picked to
answer (`LLM.Chat.Overhear.Enable`). Hearing ranges default to the server's player listen ranges
(`ListenRange.Say`/`.Yell`/`.TextEmote`); set the `LLM.*Distance` options to diverge.

## Triggers

- **Reactive chat**: say/yell, whispers, party/raid/battleground, guild, and channel messages, with
  per-channel reply chances (separate for human vs bot senders), a name-mention override, and a bot
  cap. Party chat from a real player skips the dice and the cap: every bot in the party is asked and
  the model itself decides whether the message concerns it — the prompt reminds it the whole party
  heard the message and silence is a fine answer. A real player's raid/battleground messages instead
  go through a **router**: one cheap LLM call reads the message plus a roster of the group's bots
  (class, spec, role) and picks which of them the message is meant for, up to the bot cap — so "any
  mages got water?" reaches the mage, not two random bots. Routed bots still decide for themselves
  whether to answer, with a prompt that pushes harder toward silence in large groups.
  When several bots react to one message their replies are staggered (`LLM.Chat.StaggerSeconds`),
  and each later bot's context is rebuilt when its turn comes, so it can respond to the earlier
  replies instead of echoing them. To keep bots from parroting phrases out of their own
  conversation history, the system prompt forbids reusing wording and requests carry a
  repetition penalty (`LLM.RepetitionPenalty`) that also covers prompt tokens. Faction rules are honored: unless `AllowTwoSide.Interaction.Chat` is enabled, bots don't react
  to opposite-faction speech (they couldn't understand it) and cross-faction whisper triggers are
  dropped, GMs excepted.
  Bots also hear each other: a bot's say/yell or channel message can trigger nearby bots to
  answer (the `LLM.Chat.BotReplyChance.*` dice), so bot conversations happen in front of players.
  The whole mechanism sits behind `LLM.Chat.BotTrigger.Enable`, chain-capped by
  `LLM.Chat.BotTrigger.MaxChainDepth`, and still requires a human audience.
- **Emotes**: emotes aimed at a bot, or performed nearby — including cross-faction ones, since
  text emotes are faction-agnostic for real players too.
- **Game events**: kills, deaths, level-ups, quest completions, duels, achievements, notable loot.
  Also group joins: a bot that joins a party or raid greets it in party/raid chat (this replaces
  playerbots' canned "Hello" whisper, which we keep disabled via `AiPlayerbot.EnableGreet = 0`).
- **Initiative**: an idle scheduler gives each bot periodic opportunities to act unprompted,
  with an environment description in the prompt.

Event comments and initiative remarks usually land in /say, but a configurable share
(`LLM.Event.ChannelChance`, `LLM.Initiative.ChannelChance`) goes to the bot's zone **General
channel** instead — the idle zone chatter real servers have — whenever the bot and at least one
real player are on the channel.

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

- `mod_llm_sentiment` — per bot↔player disposition, fed into prompts, adjusted by the model.
- `mod_llm_history_pair` / `mod_llm_history_room` — conversation transcripts fed into prompts.

## Configuration

See `conf/mod_llm.conf.dist` for every option (`LLM.*` namespace): endpoint/model/sampling,
concurrency, reply chances, distances, event chances/cooldowns, initiative pacing, sentiment
steps, history caps, and prompt templates.

In-game administration: `.llm status | enable | disable | reload`.

## Tests

Unit tests (tool-call parsing, schema validation, prompt assembly, mention matching) register
with the core `unit_tests` target; configure the build with `-DBUILD_TESTING=ON`.

## License

MIT (see LICENSE). Bundles nlohmann/json and cpp-httplib, both MIT.
