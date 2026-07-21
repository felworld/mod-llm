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
Today that's the bot's zone, its full party/raid roster (names and leader), guild, its own quest
log (titles plus ready-to-turn-in/failed markers, so quest talk stays honest), its own memory
notes (those about the player it's talking to, plus recent general ones), recent conversation
history, everything said in /say or /yell within earshot lately — bots overhear like players
do, whether or not they were picked to answer (`LLM.Chat.Overhear.Enable`) — and notable game
events seen nearby (duels, deaths, level-ups, ...), which land in the same overheard transcript
as narration lines, so a bow right after a duel still means something. Chat links reach prompts
as the bracketed text a player sees (`[Some Quest]`), never raw client markup. Hearing ranges default to the server's player listen ranges
(`ListenRange.Say`/`.Yell`/`.TextEmote`); set the `LLM.*Distance` options to diverge.

## Triggers

- **Reactive chat**: say/yell, whispers, party/raid/battleground, guild, and channel messages.
  Who answers is decided by judgment, not dice: most audiences go through a **router** — one cheap
  LLM call reads the message plus a roster of candidate bots (class, spec, role) and picks which of
  them, if any, the message is meant for — so "any mages got water?" reaches the mage, not two
  random bots, and idle muttering reaches nobody. A quest link is routing gold: candidates with
  the linked quest in their log are promoted ahead of the roster cap and marked for the router,
  so "anyone for [quest]?" reaches exactly the bots that could join. The say router additionally sees the conversation
  recently overheard around the sender (so an undirected "sure, how much?" right after a bot's
  offer reaches that bot), and the room router for guild and named channels sees the room
  transcript. All routers know that the bigger the crowd, the less likely any one bystander was
  being addressed; rosters are capped at `LLM.Chat.Router.MaxRoster` per call, a name-mention
  always picks that bot, and the `LLM.Chat.*ReplyChance.*` dice roll only when a router is
  disabled — an unparseable router reply routes to nobody and logs an error, so a misbehaving
  router model is loud in the logs instead of masked by fallback chatter. Party chat from a real
  player skips routing
  the other way: every bot in the party is asked and the model itself decides whether the message
  concerns it — the prompt reminds it the whole party heard the message and silence is a fine
  answer. Routed bots, too, still decide for themselves whether to answer, with a prompt that
  pushes harder toward silence in large groups.
  When several bots react to one message their replies are staggered (`LLM.Chat.StaggerSeconds`),
  and each later bot's context is rebuilt when its turn comes, so it can respond to the earlier
  replies instead of echoing them. Replies also arrive at typing speed: a finished reply is held
  until a human could have typed it (`LLM.Typing.CharsPerSecond`, counted from the message being
  answered, so model latency eats into the typing time; capped by `LLM.Typing.MaxSeconds`). Long
  messages land later than quips, one bot's lines never overlap, and burst-replies spread into a
  conversation-paced trickle — where each later reply was generated seeing the ones already
  delivered, which is what lets the model bow out once the question is answered. To keep bots from parroting phrases out of their own
  conversation history, the system prompt forbids reusing wording and requests carry a
  repetition penalty (`LLM.RepetitionPenalty`) that also covers prompt tokens. Faction rules are honored: unless `AllowTwoSide.Interaction.Chat` is enabled, bots don't react
  to opposite-faction speech (they couldn't understand it) and cross-faction whisper triggers are
  dropped, GMs excepted.
  Messages starting with the playerbots command prefix (`AiPlayerbot.CommandPrefix`) are bot
  commands, not conversation: they trigger no replies and stay out of every transcript.
  Bots also hear each other: a bot's say/yell or channel message routes exactly like a real
  player's, so bot conversations happen in front of players — but a bot's message picks at most
  `LLM.Chat.BotTrigger.MaxBotsToPick` (default 1) responder, keeping bot-to-bot exchanges linear
  conversations rather than branching trees. Only a real player's message fans out to several
  responders (`LLM.Chat.MaxBotsToPick`). The whole mechanism sits behind
  `LLM.Chat.BotTrigger.Enable`, chain-capped by `LLM.Chat.BotTrigger.MaxChainDepth`, and still
  requires a human audience. LocalDefense/WorldDefense route like everything else, with an
  alarm-channel note in the routing prompt: nearly every alarm is read in silence, but "omw" and
  sightings survive, and the picked bots get a go-or-stay-silent reply guidance with the
  `go_defend` tool — a bot that is staying put says nothing, so declines never reach the channel.
  (Per-candidate dice could never keep a faction-wide channel quiet — a low chance across hundreds
  of readers still answers every message.)
- **Emotes**: emotes aimed at a bot, or performed nearby — including cross-faction ones, since
  text emotes are faction-agnostic for real players too. A cross-faction emote normally draws an
  emote back (the prompt explains the language barrier); a small dice roll
  (`LLM.Chat.CrossFactionChatChance`) occasionally lets the bot type at the enemy anyway, which
  lands as the classic untranslated-gibberish taunt.
- **Game events**: kills, deaths, level-ups, quest completions, duels, achievements, notable loot.
  A comment about a groupmate's deed goes to party/raid chat; enemy-faction deeds draw comment
  only on the same cross-faction dice. Whether or not a bot is picked to react, the event is
  narrated into every nearby bot's overheard transcript (mob kills exempt — grinding would flood
  it), because seeing and reacting are different things. Duel events address participants in the
  second person — "you lost a duel against X" — since a small model that only sees its own name
  in a third-person line may not realize it was the loser, and trash-talk accordingly. Also group joins: a bot that joins a party or raid greets
  it in party/raid chat (this replaces playerbots' canned "Hello" whisper, which we keep disabled
  via `AiPlayerbot.EnableGreet = 0`). And heals: a bot healed by a player outside its group thanks
  them aloud (`LLM.Event.Chance.Healed`; groupmate heals are routine and never draw thanks;
  mod-playerbots adds the /thank emote, and buff-capable bots buff back whoever buffs them).
- **Initiative**: an idle scheduler gives each bot periodic opportunities to act unprompted,
  with an environment description in the prompt.

Event comments and initiative remarks usually land in /say — which happens only with a human in
actual earshot, here and wherever else a bot speaks aloud — but a configurable share
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

- `mod_llm_memory` — the per-bot scratchpad: short notes the model writes itself (`remember` /
  `forget`), keyed by slug, optionally scoped to one player. Long-term continuity lives here —
  "ninja'd my loot in deadmines" carries more than the 0..1 sentiment float it replaced.
- `mod_llm_history_pair` / `mod_llm_history_room` — conversation transcripts fed into prompts.
  Short-retention working memory for coherence; anything worth keeping belongs in a note.
  Transcripts are recency-aware: room and overheard lines older than
  `LLM.History.ScrollbackSeconds` are dropped from prompts — like chat that has scrolled off the
  chat window, it no longer exists for the player — and stale pair lines carry an age tag
  ("(10 min ago)"), so a bot greets an old acquaintance instead of resuming a dead conversation
  mid-sentence.

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
