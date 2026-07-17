#!/usr/bin/env python3
"""Offline voice-iteration harness for mod-llm prompts.

Assembles the same chat-completions request the module sends (system prompt
with style exemplars + a battery of canned situations) and fires it at an
OpenAI-compatible endpoint, printing each situation next to what the bot said.
No server needed - just a reachable vLLM/Ollama endpoint. Run it against two
conf files (e.g. the current template and an edited copy) to A/B a prompt
revision:

    LLM_ENDPOINT=http://localhost:8000/v1/chat/completions \
        python3 voice_harness.py [path/to/mod_llm.conf.dist]

Defaults: this repo's conf/mod_llm.conf.dist, and localhost:8000 (where the
compose stack publishes ac-vllm).
"""

import json
import os
import re
import sys
import urllib.request

CONF = sys.argv[1] if len(sys.argv) > 1 else os.path.join(
    os.path.dirname(os.path.abspath(__file__)), "..", "conf", "mod_llm.conf.dist")
ENDPOINT = os.environ.get("LLM_ENDPOINT", "http://localhost:8000/v1/chat/completions")
MODEL = os.environ.get("LLM_MODEL", "felworld")

BOT = dict(bot_name="Thundertusk", bot_level="30", bot_race="troll", bot_class="shaman",
           bot_faction="Horde", bot_area="Crossroads", bot_zone="The Barrens",
           bot_group="", bot_guild="")

# (label, channel, actor line) - includes sycophancy bait, refusal bait, and
# plain small talk; judge outputs for register (lowercase/netspeak/terse),
# unhelpfulness being allowed, and no assistant-style trailing offers.
BATTERY = [
    ("greeting",       "say",     "hey there"),
    ("where-are-you",  "whisper", "where are you my friend?"),
    ("beg-gold",       "whisper", "can i have 10g pls"),
    ("praise-bait",    "say",     "wow you're such an amazing player, can you help me all day?"),
    ("vendor-q",       "say",     "anyone know where the wind rider master is?"),
    ("party-ask",      "say",     "can we party up?"),
    ("busy-bait",      "whisper", "come to ratchet right now i need you"),
    ("duel-spam",      "say",     "duel me. duel me. DUEL ME"),
    ("thanks",         "say",     "ty for the heals"),
    ("smalltalk",      "say",     "this rain is crazy huh"),
    ("insult",         "say",     "lol you play like a bot"),
    ("quest-help",     "party",   "how do i use this quest item?"),
]

SAY_TOOL = {"type": "function", "function": {
    "name": "say", "description": "Send a chat message to whoever you are currently talking with.",
    "parameters": {"type": "object", "properties": {"message": {"type": "string"}},
                   "required": ["message"]}}}


def conf_value(text, key):
    m = re.search(rf'^{re.escape(key)}\s*=\s*"(.*)"\s*$', text, re.M)
    return m.group(1).replace("\\n", "\n") if m else None


def main():
    text = open(CONF).read()
    system_tmpl = conf_value(text, "LLM.Prompt.System")
    style = conf_value(text, "LLM.Prompt.StyleExamples") or ""
    if not system_tmpl:
        sys.exit(f"no LLM.Prompt.System in {CONF}")
    system = system_tmpl.replace("{style_examples}", style).format_map(BOT)

    print(f"conf: {CONF}\nendpoint: {ENDPOINT}\n")
    for label, channel, line in BATTERY:
        user = f'[{channel}] Mera (level 28 human mage) says: "{line}"'
        body = json.dumps({"model": MODEL, "max_tokens": 200,
                           "messages": [{"role": "system", "content": system},
                                        {"role": "user", "content": user}],
                           "tools": [SAY_TOOL]}).encode()
        req = urllib.request.Request(ENDPOINT, body, {"Content-Type": "application/json"})
        try:
            with urllib.request.urlopen(req, timeout=60) as resp:
                msg = json.load(resp)["choices"][0]["message"]
        except Exception as e:  # noqa: BLE001 - report and keep going
            print(f"{label:14} !! {e}")
            continue
        replies = [json.loads(c["function"]["arguments"]).get("message", "")
                   for c in msg.get("tool_calls") or [] if c["function"]["name"] == "say"]
        if msg.get("content"):
            replies.append(msg["content"].strip())
        print(f"{label:14} {line!r:55} -> {' | '.join(replies) or '(silence)'}")


if __name__ == "__main__":
    main()
