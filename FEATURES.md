# mod-llm in detail

The behavior reference for mod-llm: how each trigger works and the options
that tune it. The [README](README.md) has the overview, tool table, and
architecture; every option lives in `conf/mod_llm.conf.dist` (`LLM.*`
namespace).

## Tool offering and validation

Returning no tool calls is a valid outcome — most moments deserve no reaction.
The tool list offered with each request is filtered by trigger kind *and* by
live game state — a bot is never offered `invite_to_party` for someone already
in its group (or otherwise uninvitable), nor `challenge_duel` when a duel
can't start. Executors still re-validate at execution time, since state can
change while the request is in flight. Follow-up rounds — OpenAI tool-result
messages with context and tool list rebuilt against the new game state, capped
at two per trigger — carry two things back to the model: execution errors, so
it can pick an alternative action (`LLM.ErrorFeedback.Enable`), and read-tool
results (`get_gear`, `get_inventory`), so it can answer questions about its
own gear, bags, and money with the facts in hand.

Chat links close the loop: the bot's quest log annotates every title with a
`{quest:ID}` tag and read-tool results tag every item with `{item:ID}`; when
the model copies a tag into a `say` message, the server expands it into a real
clickable hyperlink (via playerbots' `ChatHelper`), and a tag whose ID doesn't
resolve is dropped rather than sent broken. `{spell:ID}` expands too. The
model never writes raw `|H...` client markup itself.

## Prompt context

The guiding rule: anything a player would see on their screen belongs in it.
Today that's the bot's zone, its full party/raid roster (names and leader),
guild, its own quest log (titles with `{quest:ID}` link tags plus
ready-to-turn-in/failed markers, so quest talk stays honest and linkable),
its own memory notes (those about the player it's
talking to, plus recent general ones), recent conversation history, everything
said in /say or /yell within earshot lately — bots overhear like players do,
whether or not they were picked to answer (`LLM.Chat.Overhear.Enable`) — and
notable game events seen nearby (duels, deaths, level-ups, ...), which land in
the same overheard transcript as narration lines, so a bow right after a duel
still means something. Chat links reach prompts as the bracketed text a player
sees (`[Some Quest]`), never raw client markup. Hearing ranges default to the
server's player listen ranges (`ListenRange.Say`/`.Yell`/`.TextEmote`); set
the `LLM.*Distance` options to diverge.

## Reactive chat

Say/yell, whispers, party/raid/battleground, guild, and channel messages.
Who answers is decided by judgment, not dice: most audiences go through a
**router** — one cheap LLM call reads the message plus a roster of candidate
bots (class, spec, role) and picks which of them, if any, the message is meant
for — so "any mages got water?" reaches the mage, not two random bots, and
idle muttering reaches nobody. A quest link is routing gold: candidates with
the linked quest in their log are promoted ahead of the roster cap and marked
for the router, so "anyone for [quest]?" reaches exactly the bots that could
join. The say router additionally sees the conversation recently overheard
around the sender (so an undirected "sure, how much?" right after a bot's
offer reaches that bot), and the room router for guild and named channels sees
the room transcript. All routers know that the bigger the crowd, the less
likely any one bystander was being addressed; rosters are capped at
`LLM.Chat.Router.MaxRoster` per call, a name-mention always picks that bot,
and the `LLM.Chat.*ReplyChance.*` dice roll only when a router is disabled —
an unparseable router reply routes to nobody and logs an error, so a
misbehaving router model is loud in the logs instead of masked by fallback
chatter. Party chat from a real player skips routing the other way: every bot
in the party is asked and the model itself decides whether the message
concerns it — the prompt reminds it the whole party heard the message and
silence is a fine answer. Routed bots, too, still decide for themselves
whether to answer, with a prompt that pushes harder toward silence in large
groups.

Faction rules are honored: unless `AllowTwoSide.Interaction.Chat` is enabled,
bots don't react to opposite-faction speech (they couldn't understand it) and
cross-faction whisper triggers are dropped, GMs excepted. Messages starting
with the playerbots command prefix (`AiPlayerbot.CommandPrefix`) are bot
commands, not conversation: they trigger no replies and stay out of every
transcript.

### Reply pacing

When several bots react to one message their replies are staggered
(`LLM.Chat.StaggerSeconds`), and each later bot's context is rebuilt when its
turn comes, so it can respond to the earlier replies instead of echoing them.
Replies also arrive at typing speed: a finished reply is held until a human
could have typed it (`LLM.Typing.CharsPerSecond`, counted from the message
being answered, so model latency eats into the typing time; capped by
`LLM.Typing.MaxSeconds`). Long messages land later than quips, one bot's lines
never overlap, and burst-replies spread into a conversation-paced trickle —
where each later reply was generated seeing the ones already delivered, which
is what lets the model bow out once the question is answered. To keep bots
from parroting phrases out of their own conversation history, the system
prompt forbids reusing wording and requests carry a repetition penalty
(`LLM.RepetitionPenalty`) that also covers prompt tokens.

### Bot-to-bot chat

Bots also hear each other: a bot's say/yell or channel message routes exactly
like a real player's, so bot conversations happen in front of players — but a
bot's message picks at most `LLM.Chat.BotTrigger.MaxBotsToPick` (default 1)
responder, keeping bot-to-bot exchanges linear conversations rather than
branching trees. Only a real player's message fans out to several responders
(`LLM.Chat.MaxBotsToPick`). The whole mechanism sits behind
`LLM.Chat.BotTrigger.Enable`, chain-capped by
`LLM.Chat.BotTrigger.MaxChainDepth`, and still requires a human audience.

### Defense channels

LocalDefense/WorldDefense route like everything else, with an alarm-channel
note in the routing prompt: nearly every alarm is read in silence, but "omw"
and sightings survive, and the picked bots get a go-or-stay-silent reply
guidance with the `go_defend` tool — a bot that is staying put says nothing,
so declines never reach the channel. (Per-candidate dice could never keep a
faction-wide channel quiet — a low chance across hundreds of readers still
answers every message.)

## Emotes

Emotes aimed at a bot, or performed nearby — including cross-faction ones,
since text emotes are faction-agnostic for real players too. The bot reads
exactly the line the client would show it ("Soandso makes a rude gesture at
you.") for every emote the client can send: the phrase table is generated
from the client's emote DBCs by `tools/gen_text_emote_phrases.py`. An
animation- or sound-only emote (`/train`) draws no reaction — nothing was
"said". Bystander bots see the emote's target by name ("hugs Fluffy").
A cross-faction emote normally draws an emote back (the prompt explains the
language barrier); a small dice roll (`LLM.Chat.CrossFactionChatChance`)
occasionally lets the bot type at the enemy anyway, which lands as the
classic untranslated-gibberish taunt. Outgoing, the `emote` tool's schema
offers a curated slate of ~50 social and player-culture staples (`/wave`
through `/rasp` and `/golfclap`), though any real emote name the model
picks resolves.

## Game events

Kills, deaths, level-ups, quest completions, duels, achievements, notable
loot. A comment about a groupmate's deed goes to party/raid chat; enemy-faction
deeds draw comment only on the same cross-faction dice. Whether or not a bot
is picked to react, the event is narrated into every nearby bot's overheard
transcript (mob kills exempt — grinding would flood it), because seeing and
reacting are different things. Duel events address participants in the second
person — "you lost a duel against X" — since a small model that only sees its
own name in a third-person line may not realize it was the loser, and
trash-talk accordingly. Also group joins: a bot that joins a party or raid
greets it in party/raid chat (this replaces playerbots' canned "Hello"
whisper, which we keep disabled via `AiPlayerbot.EnableGreet = 0`). And heals:
a bot healed by a player outside its group thanks them aloud
(`LLM.Event.Chance.Healed`; groupmate heals are routine and never draw thanks;
mod-playerbots adds the /thank emote, and buff-capable bots buff back whoever
buffs them).

## Initiative

An idle scheduler gives each bot periodic opportunities to act unprompted,
with an environment description in the prompt.

Event comments and initiative remarks usually land in /say — which happens
only with a human in actual earshot, here and wherever else a bot speaks
aloud — but a configurable share (`LLM.Event.ChannelChance`,
`LLM.Initiative.ChannelChance`) goes to the bot's zone **General channel**
instead — the idle zone chatter real servers have — whenever the bot and at
least one real player are on the channel.

## Persistence

Two features persist to the characters DB (schema auto-applied at worldserver
startup):

- `mod_llm_memory` — the per-bot scratchpad: short notes the model writes
  itself (`remember` / `forget`), keyed by slug, optionally scoped to one
  player. Long-term continuity lives here — "ninja'd my loot in deadmines"
  carries more than the 0..1 sentiment float it replaced.
- `mod_llm_history_pair` / `mod_llm_history_room` — conversation transcripts
  fed into prompts. Short-retention working memory for coherence; anything
  worth keeping belongs in a note. Transcripts are recency-aware: room and
  overheard lines older than `LLM.History.ScrollbackSeconds` are dropped from
  prompts — like chat that has scrolled off the chat window, it no longer
  exists for the player — and stale pair lines carry an age tag
  ("(10 min ago)"), so a bot greets an old acquaintance instead of resuming a
  dead conversation mid-sentence.

---

Felworld is a non-commercial research project. It contains no game client,
assets, or proprietary code, and is not affiliated with or endorsed by
Blizzard Entertainment — see the
[project disclaimer](https://github.com/felworld/azerothcore#license-and-disclaimer).
