"""Deadlee's tools. Each one is small, checked, and safe to call from a voice request.

Toy actions (timer, alarm, nightlight, radio) aren't run here: they're validated
and handed back to the toy in the /talk reply as "actions".
"""
import json
import logging
import os
import re
import time
from dataclasses import dataclass, field
from datetime import date, datetime

import httpx

import books
import brain
import lookup
import maths
import stories
from family import _when

log = logging.getLogger("pet-bridge")

SEARX_URL = os.environ.get("PET_SEARX_URL", "http://127.0.0.1:8081/search")
WEATHER_URL = "https://api.open-meteo.com/v1/forecast"
DEFAULT_PLACE = {"place": "Melbourne", "lat": -37.81, "lon": 144.96}

LAMP_COLOURS = ["red", "amber", "blue", "white"]          # the toy's order (settings lamp_color)
LAMP_MINUTES = {15: 1, 30: 2, 60: 3, 0: 4}                  # minutes -> settings lamp_timer (0 = always)
ALARM_DAYS = {"once": 0, "daily": 0x7F, "weekdays": 0x1F, "weekends": 0x60}
WEEKDAYS = ["monday", "tuesday", "wednesday", "thursday", "friday", "saturday", "sunday"]


def _fn(name: str, desc: str, props: dict, required: list[str]) -> dict:
    return {"type": "function", "function": {"name": name, "description": desc, "parameters": {
        "type": "object", "properties": props, "required": required}}}


TOOLS = [
    _fn("weather", "Get the weather forecast for home. Use for any weather, rain, hot/cold or "
        "what-to-wear question, and for when the sun comes up or goes down. when: now, today, "
        "tomorrow, a day of the week (the next one), or week for the next seven days.",
        {"when": {"type": "string", "enum": ["now", "today", "tomorrow", *WEEKDAYS, "week"]}}, ["when"]),
    _fn("web_search", "Look something up on the internet: news, sport results, anything recent or "
        "that the other tools don't cover. Not for weather (use weather) or family dates (you "
        "already know those).",
        {"query": {"type": "string", "description": "a short search query"}}, ["query"]),
    _fn("encyclopedia", "Look up what something is: an animal, a planet, a place, a person from "
        "history, how something works. Gives a short, plain explanation.",
        {"topic": {"type": "string", "description": "the thing to look up, e.g. 'blue whale'"}}, ["topic"]),
    _fn("dictionary", "Get how a word is spelled, letter by letter, and what it means. Always use it "
        "when asked to spell a word or what a word means.",
        {"word": {"type": "string"}}, ["word"]),
    _fn("public_holidays", "The next public holidays at home, with the days already counted. Use "
        "for 'is Monday a holiday?' or 'when is the next day off?'.", {}, []),
    _fn("joke", "Get a joke to tell. Give a topic if one was asked for.",
        {"topic": {"type": "string", "description": "one word, e.g. 'dinosaur'; leave out for any joke"}}, []),
    _fn("read_book", "Read a book from the family's bookshelf aloud, one page at a time. Use it when "
        "asked to read a book or a story, to keep reading, or to start a book again. Leave title out "
        "to carry on with the last book. from: bookmark carries on where it stopped, start goes back "
        "to the beginning, again repeats the page just read, back goes to the page before it, forward "
        "skips a page, list just says which books there are. Or give a page or chapter number to "
        "jump straight there.",
        {"title": {"type": "string", "description": "the book, e.g. 'Peter Rabbit'"},
         "from": {"type": "string", "enum": ["bookmark", "start", "again", "back", "forward", "list"]},
         "page": {"type": "integer", "minimum": 1, "description": "jump to this page"},
         "chapter": {"type": "integer", "minimum": 1, "description": "jump to the start of this chapter"}}, []),
    _fn("story", "Make up a brand new story together with the child, who helps decide what happens. Use "
        "it only when they ask to make, make up or invent a story. If they just say 'tell me a story' "
        "or 'a fairy tale', ask whether to read one from the bookshelf or make one up. Reading a "
        "book that exists (Winnie-the-Pooh, a Hans Christian Andersen tale) is read_book. action: start begins one (give the idea if they said what it is about), "
        "continue picks up a story that wasn't finished, stop ends it.",
        {"action": {"type": "string", "enum": ["start", "continue", "stop"]},
         "idea": {"type": "string", "description": "what the story is about, in their words"}}, ["action"]),
    _fn("maths_game", "Play the maths game: you ask sums at the child's level and their answers are "
        "checked for you. Use it when asked to do, play or practise maths, sums or times tables, or to "
        "be quizzed. Not for answering one sum.",
        {"action": {"type": "string", "enum": ["start", "stop"]}}, ["action"]),
    _fn("country", "Facts about a country: where it is, capital city, languages, money, how many "
        "people, how big, and its neighbours.",
        {"name": {"type": "string"}}, ["name"]),
    _fn("set_timer", "Start a countdown timer on the toy.",
        {"minutes": {"type": "integer", "minimum": 1, "maximum": 240},
         "label": {"type": "string", "description": "a few words, e.g. 'pasta' or 'teeth'"}},
        ["minutes"]),
    _fn("set_alarm", "Set the toy's wake-up alarm.",
        {"hour": {"type": "integer", "minimum": 0, "maximum": 23},
         "minute": {"type": "integer", "minimum": 0, "maximum": 59},
         "repeat": {"type": "string", "enum": list(ALARM_DAYS)}}, ["hour", "minute"]),
    _fn("nightlight", "Turn the toy's night light on or off, or change its colour, brightness or "
        "how long it stays on. Leave out anything that wasn't asked for.",
        {"on": {"type": "boolean"},
         "colour": {"type": "string", "enum": LAMP_COLOURS},
         "brightness": {"type": "integer", "minimum": 1, "maximum": 7},
         "minutes": {"type": "integer", "enum": [15, 30, 60, 0], "description": "0 = stay on"}},
        ["on"]),
    _fn("radio", "Play or stop the radio on the toy.",
        {"action": {"type": "string", "enum": ["play", "stop"]},
         "station": {"type": "string", "description": "station name if one was asked for"}},
        ["action"]),
    _fn("memory", "Save, change or remove a short note so you remember it next time. Use it when "
        "someone tells you something worth remembering (likes, favourite things, what they're "
        "into, a joke you share). target: a family member's name for notes about them, or "
        "'memory' for your own notes.",
        {"action": {"type": "string", "enum": ["add", "replace", "remove"]},
         "target": {"type": "string"},
         "content": {"type": "string", "description": "the note, under 15 words"},
         "old_text": {"type": "string", "description": "for replace/remove: a unique piece of the old note"}},
        ["action", "target"]),
    _fn("family_add", "Add a family date or plan someone tells you (a birthday, a trip, a visit) "
        "for a grown-up to check. You can't change or delete family dates.",
        {"note": {"type": "string", "description": "e.g. 'Grandma visits on 24 December'"}}, ["note"]),
    _fn("enrol_voice", "Start learning someone's voice so you recognise them later, when they ask "
        "('learn my voice, I'm Sam'). Only for family members.",
        {"name": {"type": "string"}}, ["name"]),
    _fn("cant_help", "Call this whenever you can't answer or can't do what was asked (no tool for "
        "it, a tool failed, or you're not sure), so a grown-up can add it later.",
        {"request": {"type": "string", "description": "what was asked, in a few words"},
         "why": {"type": "string"},
         "would_need": {"type": "string", "description": "what tool or info would let you help"}},
        ["request", "why"]),
]


@dataclass
class Context:
    speaker: str | None = None
    names: list[str] = field(default_factory=list)     # family names, for redaction
    actions: list[dict] = field(default_factory=list)
    used: list[str] = field(default_factory=list)
    enrol: str | None = None                            # set by enrol_voice: app.py runs the enrolment
    turn: dict | None = None                            # set by read_book, story and maths_game: app.py
                                                        # speaks this (reply, speak, speed) instead of the model
    allowed: set[str] | None = None                     # limit which tools may run (None = all)


# ---------------------------------------------------------------- weather

WMO = {0: "clear", 1: "mostly clear", 2: "partly cloudy", 3: "cloudy", 45: "foggy", 48: "foggy",
       51: "light drizzle", 53: "drizzle", 55: "heavy drizzle", 61: "light rain", 63: "rain",
       65: "heavy rain", 66: "freezing rain", 67: "freezing rain", 71: "light snow", 73: "snow",
       75: "heavy snow", 77: "snow grains", 80: "rain showers", 81: "rain showers",
       82: "heavy showers", 85: "snow showers", 86: "snow showers", 95: "thunderstorms",
       96: "thunderstorms with hail", 99: "thunderstorms with hail"}


def _place() -> dict:
    """Optional front-matter in brain/skills/weather.md: place:, lat:, lon:."""
    out = dict(DEFAULT_PLACE)
    wanted = brain.front_matter("weather")
    if wanted.get("place"):
        out["place"] = wanted["place"]
    for k in ("lat", "lon"):
        try:
            out[k] = float(wanted[k])
        except (KeyError, ValueError):
            pass
    return out


_forecast: dict = {"at": 0.0, "data": None}
FORECAST_FRESH_S = 15 * 60
FORECAST_STALE_S = 3 * 3600


def _fetch_forecast(p: dict) -> dict:
    """One call covers now and the next seven days; cached 15 min. On failure, a forecast up to
    3 h old is better than nothing (Open-Meteo is in Germany and sometimes slow)."""
    now = time.time()
    if _forecast["data"] and now - _forecast["at"] < FORECAST_FRESH_S:
        return _forecast["data"]
    params = {"latitude": p["lat"], "longitude": p["lon"], "timezone": "Australia/Melbourne", "forecast_days": 7,
              "current": "temperature_2m,apparent_temperature,weather_code,wind_speed_10m,precipitation",
              "daily": "weather_code,temperature_2m_max,temperature_2m_min,precipitation_probability_max,"
                       "uv_index_max,sunrise,sunset"}
    err: Exception | None = None
    for _ in range(2):
        try:
            r = httpx.get(WEATHER_URL, timeout=httpx.Timeout(6.0, connect=3.0), params=params)
            r.raise_for_status()
            _forecast.update(at=now, data=r.json())
            return _forecast["data"]
        except httpx.HTTPError as e:
            err = e
    if _forecast["data"] and now - _forecast["at"] < FORECAST_STALE_S:
        return _forecast["data"]
    raise err


def _clock(iso: str) -> str:
    t = datetime.fromisoformat(iso)
    return f"{t.hour % 12 or 12}:{t.minute:02d} {'am' if t.hour < 12 else 'pm'}"


def weather(when: str = "today") -> str:
    when = str(when).lower().strip()
    p = _place()
    d = _fetch_forecast(p)
    if when == "now":
        c = d["current"]
        return (f"{p['place']} now: {WMO.get(c['weather_code'], 'unknown sky')}, {round(c['temperature_2m'])} "
                f"degrees, feels like {round(c['apparent_temperature'])}, wind {round(c['wind_speed_10m'])} km/h.")
    day = d["daily"]
    days = [date.fromisoformat(t) for t in day["time"]]
    if when == "week":
        lines = [f"- {'Today' if i == 0 else days[i].strftime('%A')}: {WMO.get(day['weather_code'][i], 'unknown sky')}, "
                 f"top of {round(day['temperature_2m_max'][i])}, low of {round(day['temperature_2m_min'][i])}, "
                 f"{day['precipitation_probability_max'][i]}% chance of rain" for i in range(len(days))]
        return f"{p['place']}, the next {len(days)} days:\n" + "\n".join(lines)
    i, label = (1 if when == "tomorrow" else 0), when
    if when in WEEKDAYS:                               # the next one, which may be today
        i = next((n for n, x in enumerate(days) if x.weekday() == WEEKDAYS.index(when)), 0)
        label = _when(days[i], days[0])
    sun = ""
    if day.get("sunrise") and day.get("sunset"):
        sun = f" Sunrise {_clock(day['sunrise'][i])}, sunset {_clock(day['sunset'][i])}."
    return (f"{p['place']} {label}: {WMO.get(day['weather_code'][i], 'unknown sky')}, top of "
            f"{round(day['temperature_2m_max'][i])}, low of {round(day['temperature_2m_min'][i])} degrees, "
            f"{day['precipitation_probability_max'][i]}% chance of rain, UV {round(day['uv_index_max'][i])}.{sun}")


# ---------------------------------------------------------------- search

def web_search(query: str) -> str:
    r = httpx.get(SEARX_URL, timeout=8.0, params={"q": query[:120], "format": "json",
                                                   "safesearch": 2, "language": "en"})
    r.raise_for_status()
    results = r.json().get("results", [])[:3]
    if not results:
        return "No results."
    return "\n".join(f"- {x.get('title', '')}: {(x.get('content') or '')[:300]}" for x in results)


# ---------------------------------------------------------------- toy actions (validated)

def _int(v, lo: int, hi: int) -> int | None:
    try:
        v = int(v)
    except (TypeError, ValueError):
        return None
    return v if lo <= v <= hi else None


def toy_action(name: str, a: dict) -> tuple[dict | None, str]:
    """Check a toy action. Returns (action for the toy, result text for the model)."""
    if name == "set_timer":
        m = _int(a.get("minutes"), 1, 240)
        if m is None:
            return None, "error: minutes must be 1 to 240"
        label = re.sub(r"[^A-Za-z0-9 '\-]", "", str(a.get("label") or ""))[:20].strip()
        return {"type": "timer", "seconds": m * 60, "label": label}, f"ok, {m} minute timer started"
    if name == "set_alarm":
        h, m = _int(a.get("hour"), 0, 23), _int(a.get("minute"), 0, 59)
        rep = str(a.get("repeat") or "once").lower()
        if h is None or m is None or rep not in ALARM_DAYS:
            return None, "error: need hour 0-23, minute 0-59, repeat once/daily/weekdays/weekends"
        return {"type": "alarm", "h": h, "m": m, "days": ALARM_DAYS[rep]}, f"ok, alarm set for {h:02d}:{m:02d} ({rep})"
    if name == "nightlight":
        act: dict = {"type": "nightlight", "on": bool(a.get("on", True))}
        if a.get("colour") is not None:
            c = str(a["colour"]).lower()
            if c not in LAMP_COLOURS:
                return None, f"error: colour must be one of {', '.join(LAMP_COLOURS)}"
            act["colour"] = LAMP_COLOURS.index(c)
        if a.get("brightness") is not None:
            b = _int(a["brightness"], 1, 7)
            if b is None:
                return None, "error: brightness must be 1 to 7"
            act["level"] = b
        if a.get("minutes") is not None:
            m = _int(a["minutes"], 0, 60)
            if m not in LAMP_MINUTES:
                return None, "error: minutes must be 15, 30, 60 or 0 (stay on)"
            act["timer"] = LAMP_MINUTES[m]
        return act, "ok, night light " + ("on" if act["on"] else "off")
    if name == "radio":
        action = str(a.get("action") or "").lower()
        if action not in ("play", "stop"):
            return None, "error: action must be play or stop"
        act = {"type": "radio", "play": action == "play"}
        st = re.sub(r"[^A-Za-z0-9 .'\-]", "", str(a.get("station") or ""))[:30].strip()
        if st:
            act["station"] = st
        return act, "ok, radio " + action
    return None, "error: unknown action"


TOY_ACTIONS = {"set_timer", "set_alarm", "nightlight", "radio"}


# ---------------------------------------------------------------- how fast to say it

def speed_for(used: list[str], normal: float) -> float:
    """Talking speed for a reply, given the tools it used. The top of brain/skills/lookup.md
    can give a speed per tool ("joke: 0.9"); the slowest one that applies wins."""
    wanted = brain.front_matter("lookup")
    speeds = []
    for name in used:
        try:
            speeds.append(min(max(float(wanted[name]), 0.6), 1.3))
        except (KeyError, ValueError):
            pass
    return min(speeds) if speeds else normal


# ---------------------------------------------------------------- dispatch

def run(name: str, args_json: str, ctx: Context) -> str:
    """Run one tool call from the model; always returns text for the model (never raises)."""
    try:
        a = json.loads(args_json or "{}")
        if not isinstance(a, dict):
            raise ValueError("arguments must be an object")
    except ValueError as e:
        brain.log_event("tool_error", f"{name}: bad arguments")
        return f"error: {e}"
    if ctx.allowed is not None and name not in ctx.allowed:
        return "error: that tool isn't available here"
    ctx.used.append(name)
    try:
        if name in TOY_ACTIONS:
            act, text = toy_action(name, a)
            if act:
                ctx.actions.append(act)
            else:
                brain.log_event("tool_error", f"{name}: {text}")
            return text
        if name == "weather":
            return weather(str(a.get("when") or "today"))
        if name == "web_search":
            return web_search(str(a.get("query") or ""))
        if name == "encyclopedia":
            return lookup.encyclopedia(str(a.get("topic") or ""))
        if name == "dictionary":
            return lookup.dictionary(str(a.get("word") or ""))
        if name == "public_holidays":
            return lookup.holidays()
        if name == "joke":
            return lookup.joke(str(a.get("topic") or ""))
        if name == "country":
            return lookup.country(str(a.get("name") or ""))
        if name == "maths_game":
            turn = maths.stop() if str(a.get("action") or "start") == "stop" else maths.start(ctx.speaker or "")
            if not turn:
                return "No maths game is being played."
            ctx.turn = turn
            return "ok: the maths game is talking right now. Don't ask a sum yourself: reply with just 'Okay'."
        if name == "story":
            action = str(a.get("action") or "start")
            turn = (stories.stop() if action == "stop" else stories.resume() if action == "continue"
                    else stories.start(str(a.get("idea") or ""), ctx.speaker or ""))
            if not turn:
                return "There is no story to carry on with. Ask what a new one should be about."
            ctx.turn = turn
            return "ok: the story is being told aloud right now. Don't tell it yourself: reply with just 'Okay'."
        if name == "read_book":
            title, where = str(a.get("title") or ""), str(a.get("from") or "bookmark")
            if where == "list":
                return books.shelf_text()
            try:
                page = books.read(title, where, _int(a.get("page"), 1, 100000) or 0,
                                  _int(a.get("chapter"), 1, 1000) or 0)
            except ValueError as e:
                return f"error: {e}"
            if not page:
                return ("That book isn't on the shelf. " if title.strip() else "") + books.shelf_text()
            ctx.turn = books.turn(page)
            return (f"ok: page {page['page']} of {page['pages']} of {page['title']} is being read aloud "
                    "right now. Don't read or retell it yourself: reply with just 'Okay'.")
        if name == "memory":
            target = str(a.get("target") or "memory")
            if target.lower() in ("speaker", "me", "user") and ctx.speaker:
                target = ctx.speaker
            return brain.memory_op(str(a.get("action") or ""), target, str(a.get("content") or ""),
                                   str(a.get("old_text") or ""))
        if name == "enrol_voice":
            who = brain.resolve_person(str(a.get("name") or ""))
            if not who:
                return "error: only family members from the family list can be learned"
            ctx.enrol = who
            return "ok, starting. Say you'll learn their voice now."
        if name == "family_add":
            return brain.family_add(str(a.get("note") or ""))
        if name == "cant_help":
            brain.log_gap(str(a.get("request") or ""), str(a.get("why") or ""),
                          str(a.get("would_need") or ""), ctx.names)
            return "noted for a grown-up. Tell them kindly you can't do that yet."
    except httpx.HTTPError as e:
        log.warning("tool %s failed: %s", name, e)
        brain.log_event("tool_error", f"{name}: {type(e).__name__}")
        return "error: that didn't work just now (network problem)"
    except Exception as e:  # noqa: BLE001 - a tool must never break a reply
        log.exception("tool %s crashed", name)
        brain.log_event("tool_error", f"{name}: {type(e).__name__}")
        return "error: that didn't work"
    return "error: no such tool"
