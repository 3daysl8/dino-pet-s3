"""Routines: scheduled jobs on the server and Deadlee's good-morning / goodnight lines.

brain/cron.yaml lists the jobs and their times (Melbourne time), one per line:
    nightly: 02:30        tidy the notes + write RECOMMENDATIONS.md
    today_card: 06:30     weather + family dates + a fun fact for the morning
A job runs once a day at or after its time (so a missed one catches up when the
bridge starts). Last runs are kept in brain/routines/state.json.
The family calendar is separate: it syncs every couple of hours (calendar_sync.py).
"""
import json
import logging
import os
import re
import threading
import time
from collections import Counter
from datetime import date, datetime

import brain
import calendar_sync
import family
import llm
import prompt
import tools

log = logging.getLogger("pet-bridge")

DEFAULT_CRON = {"nightly": "02:30", "today_card": "06:30"}
TIDY_AT = 0.8                       # consolidate a notes file once it's this full

# ---------------------------------------------------------------- helpers


def _read_cron() -> dict[str, str]:
    jobs = {}
    try:
        lines = open(brain.path("cron.yaml"), encoding="utf-8").read().splitlines()
    except OSError:
        return dict(DEFAULT_CRON)
    for line in lines:
        m = re.match(r"\s*([a-z_]+)\s*:\s*\"?(\d{1,2}):(\d{2})\"?\s*(?:#.*)?$", line)
        if m:
            jobs[m.group(1)] = f"{int(m.group(2)):02d}:{m.group(3)}"
    return jobs


def _state() -> dict:
    try:
        return json.load(open(brain.path("routines", "state.json")))
    except (OSError, ValueError):
        return {}


def _save_state(s: dict) -> None:
    os.makedirs(brain.path("routines"), exist_ok=True)
    with open(brain.path("routines", "state.json"), "w") as f:
        json.dump(s, f)


def _work(system: str, user: str, max_tokens: int = 600) -> str:
    """A background call to Gemma: no tools, persona not needed."""
    return llm.local(system, [{"role": "user", "content": user}], tools.Context(), use_tools=False,
                     max_tokens=max_tokens)


# ---------------------------------------------------------------- nightly: tidy notes

TIDY_PROMPT = """You tidy the memory notes of Deadlee, a friendly helper who lives in a toy and \
talks with a family (a 6-year-old called Sam and his parents). You get one list of notes. \
Merge duplicates, keep the most useful and most recent things, and make the list shorter so it \
uses at most {limit} characters in total. Each note stays under 15 words. Never keep passwords, \
addresses, phone numbers or school names. Return only a JSON array of strings."""


def tidy_notes() -> list[str]:
    """Consolidate any notes file that's getting full. Returns what was tidied."""
    done = []
    targets = [("memory", brain.path("MEMORY.md"), brain.MEMORY_CAP)]
    targets += [(n, brain.person_file(n), brain.person_cap(n)) for n in brain.people()]
    for target, p, cap in targets:
        text = brain._read(p)
        if len(text) < cap * TIDY_AT:
            continue
        notes = (brain._entries(text) if target == "memory" else brain._notes_split(text)[1])
        if len(notes) < 3:
            continue
        out = _work(TIDY_PROMPT.replace("{limit}", str(int(cap * 0.6))), json.dumps(notes), 800)
        try:
            new = json.loads(out[out.index("["): out.rindex("]") + 1])
            new = [" ".join(str(n).split())[:150] for n in new if str(n).strip()]
        except ValueError:
            log.warning("tidy %s: unreadable reply, left as is", target)
            continue
        if new and brain.replace_notes(target, new):
            done.append(f"{target}: {len(notes)} -> {len(new)} notes")
    return done


# ---------------------------------------------------------------- nightly: recommendations

DIGEST_PROMPT = """You help a parent improve Deadlee, a voice helper for their family (mostly their \
6-year-old Sam). Below are things Deadlee couldn't do in the last week, and a catalogue of \
possible new skills. Group similar requests. For each group, suggest the best fix: a skill from \
the catalogue, or something new if nothing fits. Write Markdown only, in this form, most-asked \
first, at most 6 items:

### <short name of the gap> (<count> times)
- Examples: "<one or two requests>"
- Suggested fix: <skill and what it adds>
- Effort: small / medium / large
- Privacy: <local only, or what would leave the house>

If the list is empty, write "Nothing new this week." """


def build_digest(now: float | None = None) -> str:
    now = now or time.time()
    since = now - 7 * 86400
    gaps = brain.read_jsonl("gaps.jsonl", since)
    events = brain.read_jsonl("events.jsonl", since)
    lines = [f"# What Deadlee couldn't do (week to {datetime.now(prompt.TZ):%a %d %b %Y})", ""]
    if gaps:
        try:
            catalog = open(brain.path("skills", "catalog.md"), encoding="utf-8").read()
        except OSError:
            catalog = ""
        listing = "\n".join(f"- {g.get('request', '')} (why: {g.get('why', '')}; would need: "
                            f"{g.get('would_need', '')})" for g in gaps[-80:])
        try:
            lines.append(_work(DIGEST_PROMPT, f"Requests:\n{listing}\n\nCatalogue:\n{catalog}", 1200).strip())
        except llm.LocalError as e:
            lines.append(f"(Couldn't write suggestions tonight: {e}.) Raw requests:\n{listing}")
    else:
        lines.append("Nothing new this week.")
    counts = Counter(e.get("kind") for e in events)
    lines += ["", "## Reliability", ""]
    labels = {"tool_error": "Tool errors", "local_error": "Local model errors", "fallback": "Answered by the Claude backup",
              "tool_skipped": "Tool skipped then retried", "unsure": "'Not sure' answers",
              "long_reply": "Replies over the word limit", "empty_reply": "Empty replies"}
    for kind, label in labels.items():
        lines.append(f"- {label}: {counts.get(kind, 0)}")
    errs = Counter(e.get("detail", "").split(":")[0] for e in events if e.get("kind") == "tool_error")
    if errs:
        lines.append("- Tool errors by tool: " + ", ".join(f"{k} {v}" for k, v in errs.most_common()))
    return "\n".join(lines) + "\n"


def nightly() -> None:
    tidied = tidy_notes()
    if tidied:
        log.info("nightly tidy: %s", "; ".join(tidied))
        brain.log_event("tidy", "; ".join(tidied))
    with open(brain.path("RECOMMENDATIONS.md"), "w", encoding="utf-8") as f:
        f.write(build_digest())
    log.info("nightly: RECOMMENDATIONS.md written")


# ---------------------------------------------------------------- today card + rituals

FACT_PROMPT = ("Give one true, well-known, surprising fact for a 6-year-old about animals, dinosaurs, "
               "space or the human body. One short sentence, plain words, no intro.")


def today_card() -> dict:
    today = datetime.now(prompt.TZ).date()
    card = {"date": today.isoformat()}
    try:
        card["weather"] = tools.weather("today")
    except Exception as e:  # noqa: BLE001
        log.warning("today card: no weather (%s)", e)
    fam = family.parse(brain.family_text())
    card["family"] = family.view(fam, today)
    try:
        card["fact"] = prompt.tidy(_work("You share fun facts with children.", FACT_PROMPT, 80))
    except llm.LocalError:
        pass
    os.makedirs(brain.path("routines"), exist_ok=True)
    with open(brain.path("routines", "today.json"), "w") as f:
        json.dump(card, f)
    return card


def _card() -> dict:
    try:
        card = json.load(open(brain.path("routines", "today.json")))
        if card.get("date") == datetime.now(prompt.TZ).date().isoformat():
            return card
    except (OSError, ValueError):
        pass
    return today_card()


RITUALS = {
    "morning": ("It's the start of the day and your owner just woke up. Say good morning in one or two "
                "short sentences (at most 40 words): mention today's weather in a useful way (jumper? "
                "umbrella?), and anything happening today or tomorrow in the family list or the calendar. If "
                "nothing is happening, share the fun fact instead."),
    "bedtime": ("It's bedtime. Say goodnight in one or two short, calm sentences (at most 35 words). If "
                "something fun or important is happening tomorrow in the family list or the calendar, mention it."),
}


def ritual(kind: str, snap: "brain.Snapshot", state: dict, speaker: str | None = None) -> str:
    card = _card()
    system = prompt.build_system(snap.soul, private=True, knowledge=snap.knowledge)
    facts = "\n".join(f"{k}: {v}" for k, v in card.items() if k in ("weather", "fact"))
    user = (f"{RITUALS[kind]}\n\nToday's notes:\n{facts}\n\n"
            f"({prompt.state_text(state, speaker)})")
    return _work(system, user, 160)


# ---------------------------------------------------------------- scheduler

JOBS = {"nightly": nightly, "today_card": today_card}


def _loop() -> None:
    while True:
        try:
            now = datetime.now(prompt.TZ)
            state = _state()
            for job, at in _read_cron().items():
                fn = JOBS.get(job)
                if not fn or state.get(job) == now.date().isoformat() or now.strftime("%H:%M") < at:
                    continue
                log.info("routine %s starting", job)
                try:
                    fn()
                except Exception:  # noqa: BLE001 - a failed job must not stop the scheduler
                    log.exception("routine %s failed", job)
                    brain.log_event("routine_error", job)
                state[job] = now.date().isoformat()
                _save_state(state)
            calendar_sync.sync_if_due()             # every couple of hours, not once a day
        except Exception:  # noqa: BLE001
            log.exception("routine scheduler")
        time.sleep(30)


def start() -> None:
    threading.Thread(target=_loop, name="routines", daemon=True).start()
