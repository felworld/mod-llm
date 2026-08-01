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
the room transcript. Both transcript-aware routers treat a follow-up as
directed: a question or reply about something a listed bot said above routes
to that bot, so a newcomer's "how'd you die?" reaches whoever just said they
died. All routers know that the bigger the crowd, the less
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
note in the routing prompt: nearly every alarm is read in silence, and a bot
is only picked to answer when it is named or would genuinely go. The picked
bot gets go-or-stay-silent reply guidance with the `go_defend` tool, and the
contract is enforced in code, not just prompted: a channel-bound reply only
lands when a `go_defend` in the same response succeeded, so a model that
types a decline anyway still stays silent. First-hand sightings reach the
channel through the defense-callout event path instead. (Per-candidate dice
could never keep a faction-wide channel quiet — a low chance across hundreds
of readers still answers every message.)

The gate has a second half: no other trigger can speak into a defense
channel at all. The `say` tool's `channel` destination lets the model name
any channel the bot has joined — and every bot sits in LocalDefense and
WorldDefense — so a kill brag or idle remark aimed there (including
`channel_name: "World"`, which prefix-matches the joined "WorldDefense") is
swallowed unless the trigger itself came from that defense channel. Alarm
speech enters only through playerbots' level-gated callout system: a bot
that outclasses the intruder is sent to fight, never to shout. The callout
prompt states what was actually seen — an enemy attacking a named friendly,
hitting the area's NPCs, or an already-reported ganker merely prowling — so
the raised alarm matches the events instead of framing every sighting as
"attacking" whatever spot the witness happens to stand in.

### Battleground play calls

In Warsong Gulch, a callout in battleground chat — "inc!!", "fc mid", "get
their flag carrier" — can actually change what bots do. The group router's
prompt carries a battleground note (play callouts concern the whole team,
so route them to teammates who would act, not to nobody), and a routed
bot's prompt carries what the scoreboard would show a player: the capture
score and both flags' status with carrier names, because a callout is
situational — "inc" usually means enemies closing on your flag room, while
"fc mid" points at whichever flag carrier the flag states make relevant.
Alongside the facts comes the key to the shorthand and the `bg_strategy`
tool with four plays: `attack_fc` (hunt the enemy carrying your flag),
`attack_base` (push their flag room), `defend_fc` (escort your carrier),
`defend_base` (hold your own flag room).

The tool wraps
[playerbots' `bg strategy` order machinery](https://github.com/felworld/mod-playerbots/blob/main/FEATURES.md#warsong-gulch-teamwork)
— the same one behind the explicit `!bg strategy` chat command. The routed
bot acts as the team's interpreter, not a privileged complier: its tool
call relays the play to every bot on the team, and each of them — the
interpreter included — rolls the usual
`AiPlayerbot.BgStrategyComplianceChance`, announces the canned line when
it complies, follows the order for `AiPlayerbot.BgStrategyOrderDuration`
seconds, and throttles re-rolls, exactly as if the player had typed the
command. "inc!!" therefore moves the same ~65% of the team the explicit
command does, at the cost of one routing call plus one interpretation
call — not one LLM call per teammate. The prompt tells the model the tool
call alone is a full response, so it doesn't double-announce on top of
the compliance lines. A bot carrying the flag is never offered the tool
and is told its job is getting the flag home; `fc` plays fail with
feedback when the flag carrier they need doesn't exist. Setting
`AiPlayerbot.BgStrategyComplianceChance = 0` disables the tool along with
the command.

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
offers a curated slate of ~40 social and player-culture staples (`/wave`
through `/golfclap`), though any real emote name the model picks resolves.

## Class services

The mage and warlock favors mod-playerbots ships as explicit chat commands
([`!conjure` / `!portal` / `!ritual`](https://github.com/felworld/mod-playerbots/blob/main/FEATURES.md#class-service-commands))
are also offered to the model as tools, so a natural-language "got any
water?" or "can I get a summon?" works on an LLM bot with no command syntax:

- `conjure_refreshments` — food or water, conjured and then walked over to
  the asker for a hand-off.
- `open_portal` — a portal to a capital city the mage has learned; a model
  that guesses an unknown destination is told which cities qualify.
- `summon_player` — a real Ritual of Summoning: two nearby group members
  (or bystander bots recruited for the ritual) channel the portal with the
  warlock, and the asker gets the standard summon-accept dialog. An asker
  who isn't in the warlock's group yet (the game only lets group members
  work the portal) gets a group invite from the bot first — the ritual
  begins once they accept. The invite runs in that direction on purpose:
  a distant bot invited into *your* group teleports to you on accept,
  which defeats the summon.

The mechanics are the playerbots actions themselves — real casts and cast
times, walking into trade range, inviting the summon target, recruiting
ritual helpers — the LLM layer only decides *whether* to do the favor.
Services are favors for the bot's own circle: the asker must be a
groupmate, raidmate, or guildmate (never inside a battleground), and
they're free. Anyone else isn't offered the tools at all, so the bot
declines in character. Bots charging strangers for services is a possible
later step.

## Game events

Kills, deaths, level-ups, quest completions, duels, achievements, notable
loot. A comment about a groupmate's deed goes to party/raid chat; enemy-faction
deeds draw comment only on the same cross-faction dice. Whether or not a bot
is picked to react, the event is narrated into every nearby bot's overheard
transcript (mob kills exempt — grinding would flood it), because seeing and
reacting are different things. Duel events address participants in the second
person — "you lost a duel against X" — since a small model that only sees its
own name in a third-person line may not realize it was the loser, and
trash-talk accordingly. PvP kills are described relative to each reacting
bot's faction ("your ally X killed the enemy Y") — names carry no faction,
and a faction-blind "X killed Y" once had bots warning their own side about
an ally clearing enemy gankers. Also loot rolls: when a
group roll resolves, the winning bot may gloat and bots that Need-rolled and
lost may grumble or congratulate, in party/raid chat
(`LLM.Event.Chance.RollWon` / `.RollLost`). Most rolls draw no reaction: only
Need rolls on items at/above `LLM.Event.LootMinQuality` roll the dice — Greed
rolls are routine — but every participant bot learns the outcome through its
overheard transcript regardless, addressed in the second person for the
winner and losers ("you lost the need roll on [X] to Y"). Rolled items skip
the generic "obtained notable loot" comment so the two paths don't talk over
each other. Also group joins: a bot that joins a party or raid
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
least one real player are on the channel. Only events that carry their own
story zone-wide (level-ups, achievements, notable loot) roll for General;
play-by-play like mob pulls, deaths, and duels is invisible to readers across
the zone and stays in local /say.

## Market trading

Bots take part in the WTS/WTB economy, with the judgment/mechanics split the
module always uses: mod-playerbots owns valuation and fulfillment (the
`!wts`/`!wtb`/`!appraise`/`!sellables`/`!sellto`/`!buyfrom` commands and the
walk-up trade-window machinery documented in
[mod-playerbots' FEATURES](https://github.com/felworld/mod-playerbots/blob/main/FEATURES.md#city-market-trading)),
and the model only decides *whether* and *how to say it*.

**Advertising.** A configurable slice of initiative opportunities
(`LLM.TradeAd.Chance`) becomes a market ad when the bot is standing in a
friendly capital (the same `IsFriendlyCapital` predicate as the
busy-capitals dwell) with a real player somewhere in the Trade channel. The prompt is seeded with the bot's actual spare stock
(things it would otherwise vendor) and actual wants (reagents it ran out of,
consumables it is low on), each priced by the deterministic layer
(mod-ah-bot-plus's jittered valuation when that module is enabled, a
vendor-price heuristic otherwise) — so the model phrases a grounded one-liner
like `WTS [Light Leather] x14 40s` or posts nothing. Most ads go to Trade; a
small share lands in zone General or plain /say
(`LLM.TradeAd.GeneralPercent`/`SayPercent`), the way players occasionally
hawk outside the channel.

**Buying and negotiation.** Nothing auto-buys. A player's ad finds its
audience through the room router: item links in channel messages keep their
ids (normalized to the `{item:ID}` tag convention), and the router appraises
a sample of candidate bots through the same deterministic layer, floating
the ones that genuinely want the item (or carry it to sell) ahead of the
roster cut with a "would buy that item" / "carries that item to sell" mark.
A picked bot calls `evaluate_offer`, which answers deterministically — is
the item an upgrade/reagent/consumable it wants, what it would pay, what it
can afford — and the model engages in chat only when the answer says to.
`list_sellables` gives it its own stock and wants with `{item:ID}` tags
ready to paste. When a specific item, amount, and price have been agreed,
`commit_trade` seals the deal; the price still has to pass the deterministic
sanity bounds and gold budget, so a sweet-talked model cannot be talked into
a scam — and deals only register against real players, so bot-to-bot
haggling stays pure ambience.

**Fulfillment and hanging around.** A committed deal is completed by the
playerbots layer: the bot walks over, opens a real trade window, places the
goods or gold, and accepts only while the counterparty's side covers the
deal. A player in another city (Trade is faction-wide) gets met halfway
around the world: the deal holds for a simulated ride, then the bot arrives
a couple hundred yards out — never where a real player could watch — and
walks in; the confirmation whisper names the bot's starting zone and asks
for a few minutes. Any Trade-channel line (and every market tool call)
stamps a short market anchor that makes the busy-capitals dwell guaranteed,
renewed on engagement and extended once a deal commits — and the deal or
anchor surfaces in the prompt as an on-screen fact ("you recently put out
trade chatter and are sticking around town", "you are making your way over
to close the deal"), so the model doesn't `travel_to` away mid-haggle; the
deterministic force underneath covers a model that ignores the hint. In LLM
sessions upstream's keyword-matched responders should be off
(`AiPlayerbot.KeywordTradeReplies = 0`) so deterministic whispers don't
double-respond next to LLM-driven buyers.

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
