"""FAMILY.md: people, birthdays, work and travel, written by a parent.

The model never does date maths: this module turns the file into a short
"today and coming up" view with the days already counted.

Format (sections are "## Name"; HTML comments are ignored):
    ## People
    - Alex (Dad)
    ## Birthdays
    - Mum: 18 Oct            (or "18 October", optional year: "2 Mar 2020")
    ## Work
    - Dad: office Tue, Thu   (free text, passed through)
    ## Travel
    - 2026-10-14 to 2026-10-16: Dad in Sydney (work)
    - 2026-12-24: Grandma visits
    ## Added by Deadlee
    - 2026-10-09: Sam says Grandma visits at Christmas
Any other section is passed through as written.
"""
import re
from dataclasses import dataclass
from datetime import date, datetime

AHEAD_DAYS = 14
ADDED = "Added by Deadlee"


@dataclass
class Family:
    people: list[str]                      # "Alex (Dad)" lines as written
    birthdays: list[tuple[str, int, int, int | None]]   # (who, month, day, year)
    work: list[str]
    travel: list[tuple[date, date, str]]
    other: dict[str, list[str]]            # other sections, incl. "Added by Deadlee"
    problems: list[str]                    # lines we couldn't read (logged, shown to the parent)


def _sections(text: str) -> dict[str, list[str]]:
    text = re.sub(r"<!--.*?-->", "", text, flags=re.S)
    out: dict[str, list[str]] = {}
    cur = None
    for raw in text.splitlines():
        line = raw.strip()
        if line.startswith("## "):
            cur = line[3:].strip()
            out.setdefault(cur, [])
        elif cur and line.startswith("- ") and line[2:].strip():
            out[cur].append(line[2:].strip())
    return out


def _parse_day(s: str) -> tuple[int, int, int | None] | None:
    s = s.strip().rstrip(".")
    for fmt in ("%d %b %Y", "%d %B %Y", "%Y-%m-%d"):
        try:
            d = datetime.strptime(s, fmt)
            return d.month, d.day, d.year
        except ValueError:
            pass
    for fmt in ("%d %b %Y", "%d %B %Y"):         # no year: parse in a leap year so 29 Feb works
        try:
            d = datetime.strptime(s + " 2000", fmt)
            return d.month, d.day, None
        except ValueError:
            pass
    return None


def _parse_iso(s: str) -> date | None:
    try:
        return date.fromisoformat(s.strip())
    except ValueError:
        return None


def parse(text: str) -> Family:
    sec = _sections(text)
    fam = Family(people=sec.pop("People", []), birthdays=[], work=sec.pop("Work", []), travel=[],
                 other={}, problems=[])
    for line in sec.pop("Birthdays", []):
        who, _, when = line.partition(":")
        d = _parse_day(when) if when else None
        if who.strip() and d:
            fam.birthdays.append((who.strip(), *d))
        else:
            fam.problems.append(f"Birthdays: {line}")
    for line in sec.pop("Travel", []):
        m = re.match(r"(\d{4}-\d{2}-\d{2})(?:\s*(?:to|-|until)\s*(\d{4}-\d{2}-\d{2}))?\s*:\s*(.+)", line)
        start = _parse_iso(m.group(1)) if m else None
        end = _parse_iso(m.group(2)) if m and m.group(2) else start
        if m and start and end and end >= start:
            fam.travel.append((start, end, m.group(3).strip()))
        else:
            fam.problems.append(f"Travel: {line}")
    fam.other = sec
    return fam


def _next_birthday(month: int, day: int, today: date) -> date:
    for year in (today.year, today.year + 1):
        try:
            d = date(year, month, day)
        except ValueError:                      # 29 Feb in a non-leap year
            d = date(year, 3, 1)
        if d >= today:
            return d
    return date(today.year + 1, month, day)


def _when(d: date, today: date) -> str:
    n = (d - today).days
    day = f"{d.strftime('%A')} {d.day} {d.strftime('%B')}"
    if n == 0:
        return f"today ({day})"
    if n == 1:
        return f"tomorrow ({day})"
    return f"{day}, in {n} days"


def view(fam: Family, today: date, speaker: str | None = None) -> str:
    """Short text for the prompt: who's who, birthdays in order, travel now and soon."""
    lines = ["## Family (from FAMILY.md, dates already worked out)"]
    if fam.people:
        lines.append("People: " + "; ".join(fam.people) + ".")
    if fam.birthdays:
        lines.append("Birthdays, soonest first:")
        upcoming = sorted(((_next_birthday(m, d, today), who, y) for who, m, d, y in fam.birthdays))
        for when, who, year in upcoming:
            age = f", turning {when.year - year}" if year else ""
            mark = " (the speaker)" if speaker and who.lower().startswith(speaker.lower()) else ""
            lines.append(f"- {who}{mark}: {_when(when, today)}{age}")
    recent = []
    for who, m, d, y in fam.birthdays:                 # had a birthday in the last week
        last = _next_birthday(m, d, date.fromordinal(today.toordinal() - 7))
        if last < today:
            n = (today - last).days
            age = f", turned {last.year - y}" if y else ""
            recent.append(f"{who}, {n} day{'s' if n > 1 else ''} ago{age}")
    if recent:
        lines.append("Just had a birthday: " + "; ".join(recent) + ".")
    away_now = [(s, e, t) for s, e, t in fam.travel if s <= today <= e]
    soon = [(s, e, t) for s, e, t in fam.travel if today < s <= date.fromordinal(today.toordinal() + AHEAD_DAYS)]
    if away_now:
        lines.append("Happening now:")
        for s, e, t in away_now:
            back = "" if e == s else f", until {_when(e, today)}"
            lines.append(f"- {t}{back}")
    if soon:
        lines.append(f"Coming up in the next {AHEAD_DAYS} days:")
        for s, e, t in sorted(soon):
            span = "" if e == s else f" to {_when(e, today)}"
            lines.append(f"- {t}: {_when(s, today)}{span}")
    later = [(s, e, t) for s, e, t in fam.travel if s > date.fromordinal(today.toordinal() + AHEAD_DAYS)]
    if later:
        lines.append("Later: " + "; ".join(f"{t} ({_when(s, today)})" for s, e, t in sorted(later)[:4]) + ".")
    if fam.work:
        lines.append("Work: " + "; ".join(fam.work) + ".")
    for name, items in fam.other.items():
        if items:
            lines.append(f"{name}: " + "; ".join(items[-20:]) + ".")
    return "\n".join(lines)


def names(fam: Family) -> list[str]:
    """Names and nicknames from People ("Alex (Dad)" -> ["Alex", "Dad"]), for redaction."""
    out = []
    for p in fam.people:
        out += [w for w in re.findall(r"[A-Za-z][A-Za-z'-]+", p) if w[0].isupper()]
    out += [who for who, *_ in fam.birthdays]
    return sorted(set(out), key=len, reverse=True)


def check(text: str) -> str:
    """Counts only, never the contents (tools/deadlee-brain.ps1 prints this after a push)."""
    f = parse(text)
    bad = {}
    for p in f.problems:
        sec = p.split(":", 1)[0]
        bad[sec] = bad.get(sec, 0) + 1
    out = (f"{len(f.people)} people, {len(f.birthdays)} birthdays, {len(f.travel)} trips/events, "
           f"{len(f.work)} work lines, {len(f.other)} other sections")
    if bad:
        out += "\nCouldn't read: " + ", ".join(f"{n} line(s) in ## {s}" for s, n in bad.items())
        out += "\n(Birthdays look like '- Mum: 18 Oct'; Travel like '- 2026-10-14 to 2026-10-16: Dad in Sydney')"
    else:
        out += "\nAll lines read OK."
    return out


def append_added(text: str, line: str, today: date) -> str:
    """Add a dated line under '## Added by Deadlee' (created if missing). Never removes anything."""
    entry = f"- {today.isoformat()}: {line.strip()}"
    m = re.search(rf"^## {re.escape(ADDED)}[ \t]*$", text, flags=re.M)   # the heading, not a mention
    if not m:
        return text.rstrip() + f"\n\n## {ADDED}\n{entry}\n"
    head, tail = text[:m.end()], text[m.end():]
    nxt = re.search(r"^## ", tail, flags=re.M)
    block, rest = (tail[:nxt.start()], tail[nxt.start():]) if nxt else (tail, "")
    return head + block.rstrip() + "\n" + entry + "\n" + ("\n" + rest if rest else "")


if __name__ == "__main__":
    import sys
    print(check(open(sys.argv[1], encoding="utf-8").read()))
