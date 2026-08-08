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
still means something. Inside a battleground it also includes that match's
[scoreboard](#battleground-scoreboards). Chat links reach prompts as the bracketed text a player
sees (`[Some Quest]`), never raw client markup. Hearing ranges default to the
server's player listen ranges (`ListenRange.Say`/`.Yell`/`.TextEmote`); set
the `LLM.*Distance` options to diverge.

Levels in the prompt are the ones the bot could read off its target frame.
A hostile far enough above the bot to wear a skull reaches the model as `??`
— "a ??-level undead rogue" — the same shorthand a player would use, so bots
can't quote a number the client never showed them. Friendly levels are always
exact. See mod-playerbots'
[level perception](https://github.com/felworld/mod-playerbots/blob/main/FEATURES.md#level-perception)
for the rule itself.

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
died. The room router also promotes anyone who spoke in the transcript's
recent window ahead of the roster cap — on a faction-wide channel the bot
mid-exchange with the sender would otherwise be shuffled out of a capped
roster, and a "yes please" would route to nobody. All routers know that the bigger the crowd, the less
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

LocalDefense/WorldDefense route through their own mustering prompt
(`LLM.Prompt.DefenseRouter`) instead of the generic room router: a call for
help is meant for everyone reading it, so the router's question is not "who
is being addressed" but "who answers the call". An attack report or plea for
help from a real player musters up to `LLM.Chat.Defense.MaxResponders`
defenders (default 4, most calls mustering one or two, preferring roster
characters whose level measures up); anything else — banter, questions,
all-clears — musters nobody. Every picked bot answers with the `go_defend`
tool and actually travels, but at most `LLM.Chat.Defense.MaxSpeakers`
(default 2) post an "omw" in the channel — the rest ride out without a word,
so a mustering never floods the alarm channel. The contract is enforced in
code, not just prompted: a channel-bound reply only lands when a `go_defend`
in the same response succeeded and a speaker slot is free, so a model that
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
"attacking" whatever spot the witness happens to stand in. The intruder's
level is the shouter's own reading of it, so an outmatched witness calls out
a `??`-level attacker rather than an exact number.

### Battleground scoreboards

Anything said to a bot's own team inside a battleground — team chat, and the
raid or party the match puts everyone in — brings the match's scoreboard into
its prompt. Whispers and /say don't: those are private conversations that
happen to take place in a battleground.

The cut is the same one the prompt uses everywhere: a bot gets what the
player holding that screen gets, and nothing more. In practice that means the
**world states the client is sent** — which is exactly what the battleground
HUD draws — plus what the match announces in chat:

| Map | What the bot knows |
| --- | --- |
| Warsong Gulch | Capture score, minutes left, both flags' state with carrier names |
| Arathi Basin | Resources against the win target, who holds each of the five bases, which one is being capped right now |
| Eye of the Storm | Points against the win target, who holds each tower, whether the flag is in the middle, loose, or on a named carrier |
| Alterac Valley | Reinforcements counting down, graveyards held on each side, towers burned on each side, anything under attack, captain deaths |
| Isle of Conquest | Reinforcements counting down, who holds each objective and each keep graveyard, which keep gates are down |
| Strand of the Ancients | Round and which side you're on, the clock (in round two, the time to beat), gates down or taking damage, beach graveyards held |

Win thresholds are read from the server's own config
(`Battleground.Warsong.Flags`, `Battleground.Arathi.CapturePoints`,
`Battleground.Alterac.Reinforcements`,
`Battleground.EyeOfTheStorm.CapturePoints`), so a bot quotes the target this
realm actually plays to. Capture timers stay out: the HUD shows a base as
contested, not a countdown, so the bot says "they're capping the farm", never
"forty seconds". The one fact with no world state behind it is a coarse "you
are roughly N minutes in" for the four maps with no clock on screen — the
sense of elapsed time a player has anyway, and the only pace signal those maps
give beyond the score gap.

Every map also carries its callout key: the shorthand teammates actually type
there — `bs`/`lm`/`gm` and "inc bs" in Arathi, `fr`/`be`/`dr`/`mt` in the Eye,
"rush" and "back cap" in Alterac, gate colours and "demo" in Strand — so a bot
can read its team's chat instead of guessing at it. Before the gates open the
bot gets the map, how it's won and the callout key, but no scoreboard: the
match hasn't happened yet, and a row of zeroes only invites comment on
nothing. Arenas get nothing at all.

### Battleground positions

The scoreboard says what is happening; the place is the half of a callout
only the one standing there can supply — "inc" with no place is noise.
Mid-match, the same team-audience prompt also names where the bot itself is
standing, phrased the way players name spots: "in the tunnel", "at the
Blacksmith", "out on the Field of Strife" — plus a nudge to name the spot
when calling what's happening around it. A bot that just watched the enemy
carrier run past can now originate "fc tunnel" instead of only relaying it.

Places come from hand-curated per-map tables — flag rooms, tunnels, ramps,
bases, towers, graveyards and chokepoints as named points with radii — and
the nearest one close enough wins, with sides named from the bot's
perspective ("your flag room", "their base"). Between places — on a road,
out in open ground — the bot gets nothing and keeps only the area name it
already had: a wrong specific is worse than none.

The screen test still rules. A bot's own position is the player's own
position; and in Warsong Gulch and Eye of the Storm the client has drawn
both flag carriers on the battleground map since patch 3.2, so there the
scoreboard's carrier lines also say where the carrier's dot is ("your flag
was taken by enemy Kixxle, whose dot on your map is in your tunnel").
Enemy positions otherwise stay unknown: nothing the screen doesn't show.

### Battleground play calls

In Warsong Gulch, a callout in battleground chat — "inc!!", "fc mid", "get
their flag carrier" — can actually change what bots do. The group router's
prompt carries a battleground note (play callouts concern the whole team,
so route them to teammates who would act, not to nobody), and the routed
bot's prompt carries the scoreboard above, because a callout is situational
— "inc" usually means enemies closing on your flag room, while "fc mid"
points at whichever flag carrier the flag states make relevant.
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
it complies, and follows the order for `AiPlayerbot.BgStrategyOrderDuration`
seconds, exactly as if the player had typed the command. "inc!!" therefore
moves the same ~65% of the team the explicit command does, at the cost of
one routing call plus one interpretation call — not one LLM call per
teammate. Repeating a callout re-rolls the bots not yet on the play
(bots already complying quietly keep at it; a short guard absorbs the
same message being interpreted by two routed bots at once), so calling
again rallies more of the team. Each relay logs how many bots complied
at INFO in the `playerbots` category. The prompt tells the model the tool
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
Conjured refreshments are favors for the bot's own circle: the asker must
be a groupmate, raidmate, or guildmate (never inside a battleground), and
they're free. Anyone else isn't offered the tool at all, so the bot
declines in character.

**Portals and summons are also for sale.** The circle still rides free,
but for a real player outside it the same two tools switch to the paid
path mod-playerbots provides (the trade-deal machinery behind
`!portal`/`!ritual` for strangers — quote whispered deterministically, tip
collected through a real trade window, `AiPlayerbot.ClassService.*Tip`):

- `open_portal` registers the deal and reports the quote back to the
  model; the bot walks over — from another city if need be, with the same
  simulated ride as a cross-city trade — takes the coin in a trade
  window, and casts. No payment, no portal.
- `summon_player` quotes, then starts the ritual right away (the customer
  is far away by definition, so payment can't come first) and collects by
  trade once they land; a customer who skips out just lets the deal
  expire. The tool takes the place the asker named, and refuses when it
  doesn't match the warlock's current zone — a summon only ever lands at
  the warlock's feet.

A "wtb portal darn" / "wtb summon tanaris" ad in Trade finds its sellers
through the room router (see [Market trading](#market-trading)): mage
sellers are marked "sells portals for coin" and warlock sellers with their
*current zone* on every channel roster — no keyword gate; whether a
message is actually asking for a service is the router's judgment — and
one seller of each kind is reserved past the roster cap, so on a
faction-wide Trade channel there is always a seller the router *can*
pick. The router is told a summon seller only fits when their location
matches where the asker wants to go. A picked seller's own prompt states
how its trade works — the tool call is what takes a job — so it commits
via `open_portal`/`summon_player` instead of leaving a "sure thing" in
chat that sets nothing in motion. Sellers also work their services into
their own market ads. Only world (random) bots sell — never someone's alt
— and a tip set to 0 keeps that service circle-only, exactly as before.

## Game events

Kills, deaths, level-ups, quest completions, duels, achievements, notable
loot. A comment about a groupmate's deed goes to party/raid chat; enemy-faction
deeds draw comment only on the same cross-faction dice. Whether or not a bot
is picked to react, the event is narrated into every nearby bot's overheard
transcript (mob kills exempt — grinding would flood it), because seeing and
reacting are different things. Duels are the duelists' story: bystanders see
challenges and outcomes in their transcripts but never comment on them (at
gate duel spots, spectator commentary — and the reply chains it seeded —
drowned the area in "gl"/"gg" chatter), and the one spoken reaction,
the "gg" at the end, comes from the two who fought (`LLM.Event.Chance.Duel`).
Duel events address participants in the second
person — "you lost a duel against X" — since a small model that only sees its
own name in a third-person line may not realize it was the loser, and
trash-talk accordingly. A comment on a game event allows at most one chained
reply — an answering "well fought", then the exchange ends — rather than the
full bot-to-bot chain depth, which is what spiraled acknowledgements at duel
hotspots. PvP kills are described relative to each reacting
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

Inside a battleground that share goes to **team chat** (/bg) rather than
General: a match talks in team chat, while a BG zone's General channel is
shared by every concurrent match on that map and read by nobody. The bot
needs a real player on its team for the remark to be worth making (it does
not fall back to General from inside a match), and its prompt gets the same
scoreboard facts a battleground reply gets — score, flag states, carrier
names — plus a reminder that the whole team hears it and that team chat is
about the match.

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
vendor-price heuristic otherwise; quotes come in whole silver, and
sub-silver items or leveling leftovers never make the list — see
[mod-playerbots' FEATURES](https://github.com/felworld/mod-playerbots/blob/main/FEATURES.md#city-market-trading))
— plus the class services it sells to
strangers ([portals and summons](#class-services), priced by the configured
tips) — so the model phrases a grounded one-liner like
`WTS [Light Leather] x14 40s` or `wts portals 50s a head`, or posts
nothing. Only `LLM.TradeAd.MaxItems` of those tradables (3 by default)
reach the prompt, drawn at random for each ad: a model shown a full bag
hawks the whole bag, and a wall of item links is a message nobody reads,
while a fresh draw every time still works through the bag over several
ads. Most ads go to Trade; a small share lands in zone General or
plain /say (`LLM.TradeAd.GeneralPercent`/`SayPercent`), the way players
occasionally hawk outside the channel.

**Buying and negotiation.** Nothing auto-buys. A player's ad finds its
audience through the room router: item links in channel messages keep their
ids (normalized to the `{item:ID}` tag convention), and the router appraises
a sample of candidate bots through the same deterministic layer, floating
the ones that genuinely want the item (or carry it to sell) ahead of the
roster cut with a "would buy that item" / "carries that item to sell" mark.
Service asks need no item link at all: [sellers](#class-services) are
marked on every roster with one of each kind reserved past the cap, and
the router judges whether the message is asking.
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

## Guild recruiting

Bots whose guild rank carries invite rights recruit the way players do. The
`guild_invite` tool covers the reactive case — someone asks to join or
clearly wants in — and goes through the core's `HandleInviteMember`, so rank
rights, faction, and existing membership are all enforced (and pre-checked,
so the model gets a usable error instead of a silent no-op). Two initiative
slices add the unprompted side:

**Recruitment ads.** A slice of initiative opportunities
(`LLM.GuildAd.Chance`) becomes a recruitment line into the city's
**GuildRecruitment channel** when the bot is standing in a friendly capital
with a real player on the channel — and since the client auto-joins only
unguilded players to that channel in cities, a listener is by definition
recruitable. The prompt is seeded with real guild facts (name, member count,
online count, MOTD), so the ad only claims what is true.

**Cold pitches.** A rarer slice (`LLM.GuildRecruit.Chance`) fires when the
closest passing real player within say range is guildless and invitable: the
bot says one short friendly line and may follow it with a real invite —
chatty by design, not a silent invite out of nowhere. Firing the pitch puts
that player on a cooldown shared by *every* bot
(`LLM.GuildRecruit.CooldownSeconds`, default 30 minutes), so declining or
ignoring one invite never summons a parade of follow-up recruiters. Asking
to join a bot's guild yourself is reactive and unaffected by the cooldown.

## Guild flavors

Every guild reading the same makes a world of bot guilds feel like one guild
copied a hundred times. So each **bot-led** guild carries a persistent
identity — an ordered set of flavor tags — that shapes how its members talk
about it. Speech level only: what the playerbots underneath actually go and
do is unchanged.

The curated tags are `leveling`, `raiding`, `pvp` (battlegrounds), `wpvp`
(world PvP), `rp` (in character), and `social`. Order carries meaning: the
first tag is the guild's lead identity, the rest colour it — `rp+wpvp` is a
roleplay guild that also fights in world PvP, `wpvp+rp` the other way round.

**Assignment.** The first time a guild is seen with somebody online, its
leader decides: a guild led by a random bot rolls a profile from the weighted
`LLM.GuildFlavor.Profiles` list; a player-founded guild never gets one. The
roll is persisted to the characters DB, so retuning the weights only affects
guilds flavored from then on, and a config entry with an unknown tag or a bad
weight is logged and skipped rather than taking the option down. The default
weights lean the way a levelling realm actually looks — social/levelling
guilds common, raiding and roleplay the minority.

**Message of the day.** A freshly flavored guild with no MOTD of its own gets
one stamped from fixed per-flavor copy (a couple of classic pairings, like
`rp+wpvp`, have bespoke lines rather than composed ones). One-time and never
an overwrite: whatever a guild already says for itself wins. The stamp needs
the guild leader online — only their session may set it — so it lands on the
first pass after they log in.

**What it changes in the prompts.** All of it is composed from the tags, so
there is no per-combination copy matrix:

- **Identity, everywhere.** The member line becomes "You are a member of
  \<X\>, a roleplay guild that also fights in world PvP." — in every member's
  prompt, on every trigger.
- **Guild chat register.** Guild chat is the one room the guild's own
  identity sets the register in: `raiding` trends toward progression, gear,
  and lockouts; `wpvp` toward sightings and who is riding out; `rp` puts guild
  chat in character wherever it sits in the profile.
- **Recruiting.** The grounded guild-facts block gains a `flavor:` line the
  ads may claim, plus per-tag pitch guidance so one guild's ads stop reading
  like every other guild's. Grounding holds: copy sells what bots observably
  do, and raiding sells *ambition* rather than raid nights nobody keeps.
- **Reactive invites.** An unguilded player talking to a bot whose rank can
  invite gets the same identity, honestly described. No gatekeeping — anyone
  may ask, and the invite still runs through the core's validation.
- **Cold-pitch level gate.** A profile without `leveling` is pitching endgame,
  which a lowbie cannot take up, so those bots skip passersby below roughly
  the last quarter of the climb (56 of 80; the threshold follows the realm's
  level cap).

Administration: `.llm guildflavor ["Guild Name"|id]` shows a profile (with no
argument, the selected player's guild), `.llm guildflavor set "Guild Name"
rp+wpvp` overrides it, `clear` drops it, and `reroll` rolls a new one from the
configured weights. Options: `LLM.GuildFlavor.Enable`,
`LLM.GuildFlavor.Profiles`.

## Persistence

Three features persist to the characters DB (schema auto-applied at worldserver
startup):

- `mod_llm_memory` — the per-bot scratchpad: short notes the model writes
  itself (`remember` / `forget`), keyed by slug, optionally scoped to one
  player. Long-term continuity lives here — "ninja'd my loot in deadmines"
  carries more than the 0..1 sentiment float it replaced.
- `mod_llm_guild_flavor` — one row per flavored guild: the canonical tag
  string (`rp+wpvp`) its members talk in. Persisted so a guild keeps the
  identity it was founded with even after the profile weights are retuned.
- `mod_llm_history_pair` / `mod_llm_history_room` — conversation transcripts
  fed into prompts. Short-retention working memory for coherence; anything
  worth keeping belongs in a note. Transcripts are recency-aware: room and
  overheard lines older than `LLM.History.ScrollbackSeconds` are dropped from
  prompts — like chat that has scrolled off the chat window, it no longer
  exists for the player — and stale pair lines carry an age tag
  ("(10 min ago)"), so a bot greets an old acquaintance instead of resuming a
  dead conversation mid-sentence.

## Observability metrics

When the core's `Metric.Enable` is on (the Felworld obs stack — see the
[hub FEATURES.md](https://github.com/felworld/azerothcore/blob/main/FEATURES.md#observability)),
the module feeds the Grafana LLM dashboard; with metrics off, every emission
is a no-op:

- **Requests** — per-request latency tagged `ok` / `http_error` /
  `parse_error`, prompt and completion token counts from the endpoint's
  `usage` block, dropped requests (queue cap hit), and periodic gauges for
  endpoint availability, queue depth, and cumulative completed/failed
  totals.
- **Conversations** — each completed request counts against its
  bot-to-bot chain depth (0 = triggered by a game event or a real player),
  giving the conversation-depth histogram.
- **Tools** — every tool invocation counts `llm_tool_calls` (tool name +
  ok/error) and writes an `llm_tool` row (arguments + outcome) to the
  characters DB `felworld_events` table, which the Character Inspector
  dashboard replays per character alongside the memory scratchpad and
  conversation history above.
- **Chat routing** — every real-player message leaves an `llm_route` point
  tagged with the trigger (`say` / `party` / `guild` / `channel` /
  `defense`) and the outcome: `no_candidates` (nobody eligible to hear it),
  `to_router` / `dice_picked` / `dice_silent` at the selection stage, then
  `router_picked` / `router_silent` / `router_error` when the router's
  verdict lands (value = bots involved). Each point is paired with a log
  line on `module.llm` (INFO under `LLM.Debug.Enable`, else DEBUG) that
  names the sender, message, and — for channels — how many bots are members
  versus eligible, so a single grep reconstructs why a message did or
  didn't reach a bot (felworld/mod-llm#37).

### Exchange trace

Independently of the metrics toggle, every LLM exchange is recorded in full
to `mod_llm_trace` in the characters DB: the exact request body sent (system
prompt, context, history, sampling parameters), the raw response, and the
say-text extracted from it — for main requests and router calls alike, tagged
with trigger kind, chain depth, follow-up round, status, and latency. The
point is diagnosing voice problems from playtesting with hard data: when a
bot says something odd, the prompt that produced it is already captured. The
Character Inspector dashboard lists a bot's recent exchanges, and the LLM
dashboard's *find utterance* search pulls up every exchange whose output
contains a phrase, prompt attached. Rows are purged after
`LLM.Trace.RetentionDays` (default 14; 0 keeps everything).

---

Felworld is a non-commercial research project. It contains no game client,
assets, or proprietary code, and is not affiliated with or endorsed by
Blizzard Entertainment — see the
[project disclaimer](https://github.com/felworld/azerothcore#license-and-disclaimer).
