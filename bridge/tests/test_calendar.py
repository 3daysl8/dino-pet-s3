from datetime import date, datetime, timedelta

import brain
import calendar_sync
from prompt import TZ


def ics(today: date) -> str:
    tomorrow, later = today + timedelta(days=1), today + timedelta(days=3)
    return "\r\n".join([
        "BEGIN:VCALENDAR", "VERSION:2.0", "PRODID:-//test//EN",
        "BEGIN:VEVENT", "UID:1", "SUMMARY:Swimming   lesson",
        f"DTSTART;TZID=Australia/Melbourne:{tomorrow:%Y%m%d}T153000",
        f"DTEND;TZID=Australia/Melbourne:{tomorrow:%Y%m%d}T160000",
        "RRULE:FREQ=DAILY;COUNT=5", "DESCRIPTION:Bring the secret goggles", "LOCATION:12 Hidden St",
        "END:VEVENT",
        "BEGIN:VEVENT", "UID:2", "SUMMARY:Grandma visits",
        f"DTSTART;VALUE=DATE:{later:%Y%m%d}", f"DTEND;VALUE=DATE:{later + timedelta(days=1):%Y%m%d}",
        "END:VEVENT",
        "BEGIN:VEVENT", "UID:3", "SUMMARY:Doctor", "CLASS:PRIVATE",
        f"DTSTART;TZID=Australia/Melbourne:{tomorrow:%Y%m%d}T090000",
        f"DTEND;TZID=Australia/Melbourne:{tomorrow:%Y%m%d}T093000",
        "END:VEVENT",
        "BEGIN:VEVENT", "UID:4", "SUMMARY:Called off", "STATUS:CANCELLED",
        f"DTSTART;VALUE=DATE:{later:%Y%m%d}", f"DTEND;VALUE=DATE:{later + timedelta(days=1):%Y%m%d}",
        "END:VEVENT",
        "BEGIN:VEVENT", "UID:5", "SUMMARY:Far away",
        f"DTSTART;VALUE=DATE:{today + timedelta(days=40):%Y%m%d}",
        f"DTEND;VALUE=DATE:{today + timedelta(days=41):%Y%m%d}",
        "END:VEVENT",
        "END:VCALENDAR", ""])


def setup(brain_dir, monkeypatch, fetch):
    (brain_dir.parent / "calendar.url").write_text("# family\nwebcal://example.test/private.ics\n")
    monkeypatch.setattr(calendar_sync, "_get", fetch)


def test_parse_keeps_titles_and_times_only():
    today = date(2026, 10, 10)
    events = calendar_sync.parse(ics(today), today)
    titles = [e["title"] for e in events]
    assert titles.count("Swimming lesson") == 5 and "Grandma visits" in titles
    assert not {"Doctor", "Called off", "Far away"} & set(titles)
    assert all(set(e) == {"start", "end", "all_day", "title"} for e in events)
    assert {"start": "2026-10-11T15:30", "end": "2026-10-11T16:00", "all_day": False,
            "title": "Swimming lesson"} in events
    utc = ("BEGIN:VCALENDAR\r\nVERSION:2.0\r\nPRODID:x\r\nBEGIN:VEVENT\r\nUID:9\r\nSUMMARY:Dentist\r\n"
           "DTSTART:20261013T000000Z\r\nDTEND:20261013T003000Z\r\nEND:VEVENT\r\nEND:VCALENDAR\r\n")
    assert calendar_sync.parse(utc, today)[0]["start"] == "2026-10-13T11:00"      # Melbourne time


def test_no_link_means_no_calendar(brain_dir):
    assert calendar_sync.urls() == [] and calendar_sync.view() == ""
    assert calendar_sync.sync()["ok"] is False
    assert "## Calendar" not in brain.snapshot().knowledge


def test_sync_then_view_words_the_days(brain_dir, monkeypatch):
    today = datetime.now(TZ).date()
    seen = []
    setup(brain_dir, monkeypatch, lambda url: seen.append(url) or ics(today))
    assert calendar_sync.sync() == {"ok": True, "events": 6, "error": ""}
    assert seen == ["https://example.test/private.ics"]
    v = calendar_sync.view()
    assert "- Swimming lesson: tomorrow (" in v and "at 3:30 pm" in v
    assert v.count("Swimming lesson") == 2 and "(and 3 more times after that)" in v
    assert "- Grandma visits:" in v and "in 3 days, all day" in v
    assert "Doctor" not in v and "goggles" not in v and "Hidden St" not in v
    know = brain.snapshot("sam").knowledge
    assert know.index("## Family") < know.index("## Calendar")
    assert calendar_sync.status()["configured"] and calendar_sync.status()["events"] == 6


def test_a_failed_sync_keeps_the_old_events(brain_dir, monkeypatch):
    today = datetime.now(TZ).date()
    setup(brain_dir, monkeypatch, lambda url: ics(today))
    calendar_sync.sync()

    def down(url):
        raise TimeoutError("https://example.test/private.ics timed out")
    monkeypatch.setattr(calendar_sync, "_get", down)
    assert calendar_sync.sync() == {"ok": False, "events": 6, "error": "TimeoutError"}
    assert "Swimming lesson" in calendar_sync.view() and calendar_sync.status()["error"] == "TimeoutError"
    events = brain.read_jsonl("events.jsonl")
    assert events[-1]["kind"] == "calendar_error" and "example" not in str(events)


def test_past_events_drop_out_of_the_view(brain_dir, monkeypatch):
    today = datetime.now(TZ).date()
    setup(brain_dir, monkeypatch, lambda url: ics(today))
    calendar_sync.sync()
    later = datetime.combine(today + timedelta(days=4), datetime.min.time(), TZ)
    v = calendar_sync.view(later)
    assert "Grandma visits" not in v and "Swimming lesson: today (" in v
