"""Calendar sync: the family calendar's next two weeks, so Deadlee knows what's on.

    /data/calendar.url        one private ICS link per line (Google, iCloud, Fastmail...;
                              "#" lines are comments). Lives next to the keys, never in
                              the brain folder or git. No file = no calendar.
    brain/calendar.json       what the last sync found: titles and times only.

Only an event's title and times are kept: no description, place, guests or links.
Events marked private or cancelled are skipped. Like the rest of the brain, none of
it is ever sent to Claude, and this module never logs titles or links. (The bridge's
talk log line prints Deadlee's replies, which name an event when he answers a
calendar question.)
The model never does date maths: view() words the days ("tomorrow at 3:30 pm").
"""
import json
import logging
import os
import time
from datetime import date, datetime, timedelta

import httpx
import icalendar
import recurring_ical_events

import brain
from family import _when
from prompt import TZ

log = logging.getLogger("pet-bridge")

AHEAD_DAYS = 14
SYNC_EVERY_S = 2 * 3600
RETRY_S = 15 * 60
MAX_EVENTS = 80
MAX_LINES = 40
TITLE_MAX = 60
SAME_MAX = 2                        # show this many of a repeating event, then just count the rest

_last_try = 0.0


def _url_file() -> str:
    return os.path.join(brain.DATA, "calendar.url")


def urls() -> list[str]:
    try:
        lines = open(_url_file(), encoding="utf-8").read().splitlines()
    except OSError:
        return []
    out = []
    for line in lines:
        line = line.strip()
        if line and not line.startswith("#"):
            out.append("https://" + line[9:] if line.lower().startswith("webcal://") else line)
    return out


def _get(url: str) -> str:
    r = httpx.get(url, timeout=20.0, follow_redirects=True)
    r.raise_for_status()
    return r.text


def _local(d) -> tuple[str, bool]:
    """An ICS date or datetime as local ISO text, and whether it is a whole day."""
    if isinstance(d, datetime):
        if d.tzinfo is not None:
            d = d.astimezone(TZ)
        return d.replace(tzinfo=None).isoformat(timespec="minutes"), False
    return d.isoformat(), True


def parse(ics: str, today: date) -> list[dict]:
    """Events from today to AHEAD_DAYS ahead, repeats expanded: {start, end, all_day, title}."""
    cal = icalendar.Calendar.from_ical(ics)
    out = []
    for ev in recurring_ical_events.of(cal).between(today, today + timedelta(days=AHEAD_DAYS + 1)):
        if str(ev.get("CLASS", "")).upper() in ("PRIVATE", "CONFIDENTIAL"):
            continue
        if str(ev.get("STATUS", "")).upper() == "CANCELLED":
            continue
        title = " ".join(str(ev.get("SUMMARY", "")).split())[:TITLE_MAX]
        if not title or "DTSTART" not in ev:
            continue
        start, all_day = _local(ev["DTSTART"].dt)
        end = _local(ev["DTEND"].dt)[0] if "DTEND" in ev else start
        out.append({"start": start, "end": end, "all_day": all_day, "title": title})
    return out


def _load() -> dict:
    try:
        return json.load(open(brain.path("calendar.json"), encoding="utf-8"))
    except (OSError, ValueError):
        return {}


def sync() -> dict:
    """Fetch every link and replace the stored events. If any link fails, the old
    events stay. Returns {"ok", "events", "error"} with counts only."""
    global _last_try
    _last_try = time.time()
    links = urls()
    if not links:
        return {"ok": False, "events": 0, "error": "no calendar link set"}
    today = datetime.now(TZ).date()
    events, seen = [], set()
    try:
        for url in links:
            for e in parse(_get(url), today):
                key = (e["start"], e["title"].lower())
                if key not in seen:
                    seen.add(key)
                    events.append(e)
    except Exception as e:  # noqa: BLE001 - never the link or the content in a log
        kind = type(e).__name__
        log.warning("calendar sync failed (%s)", kind)
        brain.log_event("calendar_error", kind)
        old = _load()
        old["error"] = kind
        brain._write(brain.path("calendar.json"), json.dumps(old))
        return {"ok": False, "events": len(old.get("events", [])), "error": kind}
    events.sort(key=lambda e: (e["start"][:10], not e["all_day"], e["start"], e["title"]))
    del events[MAX_EVENTS:]
    brain._write(brain.path("calendar.json"), json.dumps({"synced": time.time(), "events": events}))
    log.info("calendar synced: %d events from %d link(s)", len(events), len(links))
    return {"ok": True, "events": len(events), "error": ""}


def sync_if_due() -> None:
    """Called by the routines loop: every couple of hours, sooner after a failure."""
    if not urls():
        return
    now = time.time()
    if now - _load().get("synced", 0) >= SYNC_EVERY_S and now - _last_try >= RETRY_S:
        sync()


def status() -> dict:
    data = _load()
    return {"configured": bool(urls()), "synced": data.get("synced", 0),
            "events": len(data.get("events", [])), "error": data.get("error", "")}


def _clock(t: datetime) -> str:
    hour = f"{t.hour % 12 or 12}" + (f":{t.minute:02d}" if t.minute else "")
    return f"{hour} {'am' if t.hour < 12 else 'pm'}"


def view(now: datetime | None = None) -> str:
    """Short text for the prompt. "" until a calendar has been synced."""
    data = _load()
    if "events" not in data:
        return ""
    now = (now or datetime.now(TZ)).replace(tzinfo=None)
    today = now.date()
    last = today + timedelta(days=AHEAD_DAYS)
    lines, shown = [], {}
    events = data["events"]
    for i, e in enumerate(events):
        if e["all_day"]:
            start, end = date.fromisoformat(e["start"]), date.fromisoformat(e["end"]) - timedelta(days=1)
            end = max(end, start)                      # an ICS whole-day end is the day after
            if end < today or start > last:
                continue
            if start < today:
                text = "on now" + ("" if end == today else f", until {_when(end, today)}")
            else:
                text = _when(start, today) + (", all day" if end == start else f" to {_when(end, today)}")
            key = (e["title"].lower(), "")
        else:
            start, end = datetime.fromisoformat(e["start"]), datetime.fromisoformat(e["end"])
            if max(end, start) < now or start.date() > last:
                continue
            text = f"{_when(start.date(), today)} at {_clock(start)}"
            key = (e["title"].lower(), start.strftime("%H:%M"))
        shown[key] = shown.get(key, 0) + 1
        if shown[key] > SAME_MAX:
            continue
        if shown[key] == SAME_MAX:                     # a repeating event: count the rest instead
            more = sum(1 for x in events[i + 1:] if x["title"].lower() == key[0] and
                       x["all_day"] == e["all_day"] and x["start"][11:16] == key[1] and
                       x["start"][:10] <= last.isoformat())
            if more:
                text += f" (and {more} more time{'s' if more > 1 else ''} after that)"
        lines.append(f"- {e['title']}: {text}")
    head = f"## Calendar (the family calendar for the next {AHEAD_DAYS} days, dates already worked out)"
    if not lines:
        return head + "\nNothing is in the calendar."
    return "\n".join([head] + lines[:MAX_LINES])
