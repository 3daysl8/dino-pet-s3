"""Look-up tools: free services that need no key, plus one bundled file.

    encyclopedia   Simple English Wikipedia (then English Wikipedia)
    dictionary     Simple English Wiktionary (then English Wiktionary); the spelling is done here
    holidays       Nager.Date public holidays for home (PET_HOLIDAY_REGION, default AU-VIC)
    joke           icanhazdadjoke, filtered for a 6-year-old
    country        countries.json (tools/gen_countries.py) + the World Bank for population

Only the thing being looked up leaves the house (a topic, a word, a joke subject, a
country code), never who asked. Every function returns short text for the model and
raises httpx.HTTPError on a network problem (tools.run turns that into an error line).
"""
import collections
import difflib
import html
import json
import os
import random
import re
import time
from datetime import date, datetime
from urllib.parse import quote

import httpx

from family import _when
from prompt import TZ

UA = {"User-Agent": "pet-bridge/1.0 (home family assistant)"}
HOLIDAY_REGION = os.environ.get("PET_HOLIDAY_REGION", "AU-VIC")


def _json(url: str, params: dict | None = None, headers: dict | None = None, timeout: float = 8.0):
    """GET some JSON. None for a 404 (not found is an answer, not a failure)."""
    r = httpx.get(url, params=params, headers={**UA, **(headers or {})}, follow_redirects=True,
                  timeout=httpx.Timeout(timeout, connect=3.0))
    if r.status_code == 404:
        return None
    r.raise_for_status()
    return r.json()


def _clip(text: str, n: int) -> str:
    """At most n characters, cut at the end of a sentence where there is one."""
    text = " ".join(text.split())
    if len(text) <= n:
        return text
    cut = text[:n]
    end = max(cut.rfind(". "), cut.rfind("! "), cut.rfind("? "))
    return cut[:end + 1] if end > n // 3 else cut.rsplit(" ", 1)[0] + "..."


# ---------------------------------------------------------------- encyclopedia

def encyclopedia(topic: str) -> str:
    topic = " ".join(str(topic).split())[:80]
    if not topic:
        return "error: nothing to look up"
    for site in ("simple", "en"):                      # the simple one is written for learners
        base = f"https://{site}.wikipedia.org"
        found = _json(base + "/w/rest.php/v1/search/page", {"q": topic, "limit": 1}) or {}
        pages = found.get("pages") or []
        if not pages:
            continue
        page = _json(base + "/api/rest_v1/page/summary/" + quote(pages[0]["key"], safe="")) or {}
        text = " ".join((page.get("extract") or "").split())
        if page.get("type") == "disambiguation" or len(text) < 40:
            continue
        return f"{page.get('title', topic)}: {_clip(text, 600)}"
    return "Nothing found about that. Say you're not sure, or try web_search."


# ---------------------------------------------------------------- dictionary

WORD_KINDS = {"noun", "verb", "adjective", "adverb", "pronoun", "preposition", "conjunction",
              "interjection", "determiner", "number", "proper noun", "contraction", "abbreviation"}


def _simple_wiktionary(word: str) -> list[str]:
    for title in dict.fromkeys((word, word.capitalize())):
        data = _json("https://simple.wiktionary.org/w/api.php",
                     {"action": "query", "prop": "extracts", "explaintext": 1, "redirects": 1,
                      "titles": title, "format": "json"}) or {}
        text = "".join(p.get("extract", "") for p in data.get("query", {}).get("pages", {}).values())
        out, kind, taken = [], None, 0
        for line in text.splitlines():
            line = line.strip()
            head = re.fullmatch(r"=+\s*(.+?)\s*=+", line)
            if head:
                kind = head.group(1).lower() if head.group(1).lower() in WORD_KINDS else None
                taken = 0
            elif kind and line and taken < 2 and len(out) < 4:
                tagged = re.match(r"(\([^)]*\)\s*)+", line)          # "(countable) A bat is..."
                if taken and not tagged:                           # an example sentence, not a meaning
                    taken = 2
                    continue
                taken += 1
                out.append(f"({kind}) {_clip(line[tagged.end():] if tagged else line, 160)}")
        if out:
            return out
    return []


def _wiktionary(word: str) -> list[str]:
    data = _json("https://en.wiktionary.org/api/rest_v1/page/definition/" + quote(word, safe="")) or {}
    out = []
    for entry in (data.get("en") or [])[:2]:
        kind = str(entry.get("partOfSpeech", "")).lower()
        for d in (entry.get("definitions") or [])[:2]:
            text = " ".join(html.unescape(re.sub(r"<[^>]+>", "", d.get("definition", ""))).split())
            if text:
                out.append(f"({kind}) {_clip(text, 160)}")
    return out


def spell(word: str) -> str:
    """Letter by letter, done in code: models slip when they spell aloud."""
    parts = ["-".join(w.upper()) for w in re.findall(r"[A-Za-z']+", word)]
    return ", then ".join(parts)


def dictionary(word: str) -> str:
    word = " ".join(re.sub(r"[^A-Za-z' -]", "", str(word)).split()).lower()[:40]
    if not word:
        return "error: no word given"
    meanings = _simple_wiktionary(word) or _wiktionary(word)
    if not meanings:
        return (f"'{word}' isn't in the dictionary, so it may be spelled another way. Don't guess: "
                "ask them to use it in a sentence, or say you're not sure.")
    letters = sum(c.isalpha() for c in word)
    return f"Spelling: {spell(word)} ({letters} letters). Meaning: " + " ".join(meanings)


# ---------------------------------------------------------------- public holidays

_holiday_years: dict[int, tuple[float, list[tuple[date, str]]]] = {}


def _holiday_year(year: int) -> list[tuple[date, str]]:
    at, days = _holiday_years.get(year, (0.0, []))
    if time.time() - at < 86400:
        return days
    data = _json(f"https://date.nager.at/api/v3/PublicHolidays/{year}/{HOLIDAY_REGION[:2]}") or []
    days = sorted((date.fromisoformat(h["date"]), h["localName"]) for h in data
                  if h.get("global") or HOLIDAY_REGION in (h.get("counties") or []))
    _holiday_years[year] = (time.time(), days)
    return days


def holidays(today: date | None = None) -> str:
    today = today or datetime.now(TZ).date()
    coming = [(d, n) for d, n in _holiday_year(today.year) if d >= today]
    if len(coming) < 4:
        try:
            coming += _holiday_year(today.year + 1)
        except httpx.HTTPError:
            pass
    if not coming:
        return "No public holidays found."
    lines = [f"- {name}: {_when(day, today)}" for day, name in coming[:5]]
    return "The next public holidays at home (days already counted):\n" + "\n".join(lines)


# ---------------------------------------------------------------- jokes

JOKE_URL = "https://icanhazdadjoke.com/"
NOT_FOR_KIDS = re.compile(                             # whole words, then word beginnings
    r"\b(bars?|pubs?|ass|bras?|wars?|hell|die[sd]?|dying|dead|gods?|guns?|shots?|butts?|graves?|weed|sex|"
    r"damn|crap)\b|"
    r"\b(beer|wine|whisk|vodka|drunk|booze|alcohol|naked|nude|kill|death|murder|suicid|shoot|drug|stoned|"
    r"boob|wife|wives|husband|girlfriend|boyfriend|divorc|pregnan|condom|viagra|prostitut|hooker|stripper|"
    r"cancer|funeral|corpse|cemeter|blood|bomb|terror|racis|religio|jesus|church|priest|jew|muslim|nazi|"
    r"cigar|smok|marri|sexy|sexu)\w*", re.I)
_told: collections.deque = collections.deque(maxlen=30)


def _fit(text: str) -> bool:
    return bool(text) and len(text) < 220 and not NOT_FOR_KIDS.search(text) and text not in _told


def joke(topic: str = "") -> str:
    topic = " ".join(re.sub(r"[^A-Za-z ]", "", str(topic)).split())[:30]
    json_please = {"Accept": "application/json"}
    pool, note = [], ""
    if topic:
        found = _json(JOKE_URL + "search", {"term": topic, "limit": 30}, json_please) or {}
        pool = [j.get("joke", "") for j in found.get("results", []) if _fit(j.get("joke", ""))]
        if not pool:
            note = f"(No joke about {topic}, so here is a different one.) "
    for _ in range(5):
        if pool:
            break
        text = (_json(JOKE_URL, None, json_please) or {}).get("joke", "")
        if _fit(text):
            pool = [text]
    if not pool:
        return "No joke came back just now. Tell a gentle one of your own."
    text = random.choice(pool)
    _told.append(text)
    return note + text


# ---------------------------------------------------------------- countries

NICKNAMES = {"usa": "United States", "us": "United States", "america": "United States",
             "uk": "United Kingdom", "britain": "United Kingdom", "great britain": "United Kingdom",
             "holland": "Netherlands", "uae": "United Arab Emirates", "nz": "New Zealand"}
_countries: list[dict] = []
_people: dict[str, tuple[int, str] | None] = {}


def _load() -> list[dict]:
    if not _countries:
        p = os.path.join(os.path.dirname(os.path.abspath(__file__)), "countries.json")
        _countries.extend(json.load(open(p, encoding="utf-8"))["countries"])
    return _countries


def _plain(s: str) -> str:
    return re.sub(r"^the ", "", " ".join(re.sub(r"[^a-z ]", " ", s.lower()).split()))


def find_country(name: str) -> dict | None:
    want = _plain(NICKNAMES.get(_plain(name), name))
    if not want:
        return None
    index = {}
    for c in _load():
        for n in (c["name"], c["official"], *c["also"]):
            index.setdefault(_plain(n), c)
    if want in index:
        return index[want]
    close = difflib.get_close_matches(want, list(index), n=1, cutoff=0.82)
    return index[close[0]] if close else None


def _big(n: float) -> str:
    """A number the way you'd say it."""
    for size, word in ((1e9, "billion"), (1e6, "million"), (1e3, "thousand")):
        if n >= size:
            v = n / size
            return f"{v:.1f}".rstrip("0").rstrip(".") + f" {word}" if v < 10 else f"{round(v)} {word}"
    return str(round(n))


def _population(code: str) -> tuple[int, str] | None:
    if code not in _people:
        try:
            data = _json(f"https://api.worldbank.org/v2/country/{code}/indicator/SP.POP.TOTL",
                         {"format": "json", "mrnev": 1}, timeout=4.0)
            row = data[1][0]
            _people[code] = (int(row["value"]), str(row["date"]))
        except Exception:  # noqa: BLE001 - population is a nice-to-have
            return None
    return _people[code]


def country(name: str) -> str:
    c = find_country(str(name))
    if not c:
        return "I don't have a country called that. Try the encyclopedia tool."
    where = c["subregion"] if c["subregion"] and c["name"] not in c["subregion"] else c["region"]
    parts = [f"{c['name']} is in {where or 'the world'}."]
    if c["capital"]:
        parts.append("Capital: " + " and ".join(c["capital"]) + ".")
    if c["languages"]:
        parts.append("Languages: " + ", ".join(c["languages"][:4]) + ".")
    if c["money"]:
        parts.append("Money: " + ", ".join(c["money"][:2]) + ".")
    people = _population(c["code"])
    if people:
        parts.append(f"About {_big(people[0])} people live there ({people[1]}).")
    if c["area"]:
        parts.append(f"Size: about {_big(c['area'])} square kilometres.")
    if c["borders"]:
        parts.append("Neighbours: " + ", ".join(c["borders"][:8]) + ".")
    else:
        parts.append("It has no land neighbours.")
    if c["landlocked"]:
        parts.append("It has no coast.")
    if not c["independent"]:
        parts.append("It is a territory, not an independent country.")
    return " ".join(parts)
