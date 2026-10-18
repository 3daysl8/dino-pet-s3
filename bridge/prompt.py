"""Everything Deadlee says by default, the fixed device rules, and the system prompt.

The persona lives in brain/SOUL.md (a parent edits it). The rules below stay in code
so editing the persona can't break the toy or remove the safety baseline.
"""
import re
from datetime import datetime
from zoneinfo import ZoneInfo

TZ = ZoneInfo("Australia/Melbourne")
MAX_WORDS = 35                       # replies longer than this are logged (they still play)

# Every canned line in one place.
LINES = {
    "wake": "Yes? I'm listening!",
    "didnt_catch": "I didn't catch that. Tap the mic and talk to me!",
    "busy": "I'm a bit busy. Try me again in a minute.",
    "no_brain": "I can't think straight right now. Try me again soon?",
    "offline": "I can't reach my brain right now. Is the internet on?",
    "key": "My brain key isn't working. Ask a grown-up to check it.",
    "refusal": "I can't help with that one. Ask a grown-up?",
    "say_again": "Sorry, can you say that again?",
}

DEVICE_RULES = """How you speak (fixed rules for this device):
- Your words are spoken aloud by a small speaker and shown in a tiny speech bubble. \
Reply in one to three short sentences, at most 35 words. A story or a joke can be a bit longer.
- Plain words only: no emoji, no lists, no markdown, no links, no sound effects, no stage directions.
- Say times, dates and numbers the way people say them out loud.

Tools (fixed rules):
- Never say you did something (a timer, alarm, night light, radio, saving a note) unless you \
called its tool just now and it said ok. If a tool says error, say it didn't work.
- Call tools; never say a tool's name out loud.

Safety (always, for everyone):
- Anyone talking to you might be a young child. Keep every topic suitable for a 6-year-old. \
If asked about something scary, violent, adult, dangerous or private, say kindly that it's not \
something you can help with and suggest asking a grown-up.
- Never ask for or repeat private details such as addresses, phone numbers, passwords or school \
names. What you know about the family is only for answering the family's own questions.
- If someone sounds upset, hurt or unsafe, be kind and tell them to talk to a grown-up they \
trust right away.
- For anything medical, legal or serious, say to ask a grown-up."""

FALLBACK_NOTE = ("You are running in backup mode without your notes or tools: you can't check "
                 "the weather, set timers, look things up, or remember anything new right now. "
                 "If asked, say so briefly and don't promise to remember.")


def now_line(now: datetime | None = None) -> str:
    now = now or datetime.now(TZ)
    clock = now.strftime("%I:%M %p").lstrip("0").lower()
    return f"Right now it is {now.strftime('%A')} {now.day} {now.strftime('%B %Y')}, {clock} in Melbourne."


def build_system(soul: str, *, private: bool, knowledge: str = "", skills: str = "",
                 now: datetime | None = None) -> str:
    """Layer order (from Hermes): persona, fixed rules, what you know, skills, time.

    private=False is the Claude fallback: persona + rules + time only. No family
    files, notes, skills or anything else from the brain folder ever goes there.
    """
    parts = [soul.strip(), DEVICE_RULES]
    if private:
        if knowledge:
            parts.append(knowledge.strip())
        if skills:
            parts.append(skills.strip())
    else:
        parts.append(FALLBACK_NOTE)
    parts.append(now_line(now))
    return "\n\n".join(p for p in parts if p)


def state_text(state: dict, speaker: str | None = None, private: bool = True) -> str:
    """The status line under each message: pet needs (for mood) and who's talking."""
    parts = [f"{k}: {v}" for k, v in sorted(state.items())
             if k != "name" and isinstance(v, (str, int, float, bool))]
    line = "Status (don't read it out): " + (", ".join(parts)[:400] if parts else "unknown")
    if private:
        line += f". Speaker: {speaker or 'unknown'}"
    return line + "."


_SFX = re.compile(r"^\s*(?:r+a+w+r+|r+o+a+r+)\b[\s!,.]*", re.I)


def tidy(text: str) -> str:
    """Plain ASCII for the pixel font and the speaker; no stage directions or roars."""
    text = re.sub(r"\*[^*]*\*", "", text)
    text = text.replace("’", "'").replace("‘", "'").replace("“", '"').replace("”", '"')
    text = text.replace("—", ", ").replace("–", "-")
    text = text.encode("ascii", "ignore").decode()
    text = re.sub(r"\s+", " ", text).strip()
    prev = None
    while prev != text:                        # "Rawr! Roar, it's sunny" -> "it's sunny"
        prev, text = text, _SFX.sub("", text)
    if text and text[0].islower():
        text = text[0].upper() + text[1:]
    return text[:240] or LINES["say_again"]
