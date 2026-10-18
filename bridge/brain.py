"""Deadlee's brain folder (Hermes-style): a few plain files that make it know the family.

    brain/SOUL.md            persona (a parent edits; re-read when it changes)
    brain/MEMORY.md          Deadlee's own notes                    cap 2,200 chars
    brain/people/<name>.md   one profile per person: the parent's part, then "## Deadlee's notes"
                             cap 1,375 chars for the main person (Sam), 600 for others
    brain/FAMILY.md          people, birthdays, work, travel (a parent edits; voice can only append)
    brain/skills/*.md        short notes on when/how to use each tool
    brain/calendar.json      the family calendar's next two weeks (calendar_sync.py writes it)
    brain/gaps.jsonl         requests Deadlee couldn't handle (redacted)
    brain/events.jsonl       reliability events: tool errors, fallbacks, long replies

Snapshot rule: the files are read when a conversation starts and frozen for it.
Writes go to disk straight away and show up in the next conversation.
Nothing in here is ever sent to Claude (see prompt.build_system(private=False)).
"""
import json
import os
import re
import shutil
import threading
import time
from dataclasses import dataclass, field
from datetime import datetime

import family
from prompt import TZ

DATA = os.environ.get("PET_DATA", "/data")
BRAIN = os.path.join(DATA, "brain")
TEMPLATE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "brain-template")
MAIN_PERSON = os.environ.get("PET_MAIN_PERSON", "sam").lower()

MEMORY_CAP = 2200
MAIN_PERSON_CAP = 1375
PERSON_CAP = 600
FAMILY_CAP = 8000
SOUL_CAP = 4000
SKILL_CAP = 1500                 # every skill rides along in each chat, so keep them short
CATALOG_CAP = 6000               # skills/catalog.md is only read by the nightly digest
NOTES = "## Deadlee's notes"
GAPS_MAX = 500
EVENTS_MAX = 2000

_lock = threading.Lock()


def path(*p: str) -> str:
    return os.path.join(BRAIN, *p)


def _read(p: str) -> str:
    try:
        with open(p, encoding="utf-8", errors="replace") as f:     # a file saved in another encoding still reads
            return f.read()
    except OSError:
        return ""


def _write(p: str, text: str) -> None:
    os.makedirs(os.path.dirname(p), exist_ok=True)
    tmp = p + ".tmp"
    with open(tmp, "w", encoding="utf-8") as f:
        f.write(text)
    os.replace(tmp, p)


def setup() -> None:
    """First start: copy the templates, and fold the old memory.json notes into MEMORY.md."""
    os.makedirs(path("people"), exist_ok=True)
    if os.path.isdir(TEMPLATE):
        for root, _, files in os.walk(TEMPLATE):
            for f in files:
                src = os.path.join(root, f)
                dst = path(os.path.relpath(src, TEMPLATE))
                if not os.path.exists(dst):
                    os.makedirs(os.path.dirname(dst), exist_ok=True)
                    shutil.copyfile(src, dst)
    old = os.path.join(DATA, "memory.json")
    if os.path.exists(old):
        try:
            notes = [n for n in json.load(open(old)).get("notes", []) if isinstance(n, str)]
        except (OSError, ValueError):
            notes = []
        if notes:
            text = _read(path("MEMORY.md")).rstrip() + "\n" + "".join(f"- {n}\n" for n in notes)
            _write(path("MEMORY.md"), text)
        os.replace(old, old + ".migrated")


# ---------------------------------------------------------------- reading

def soul() -> str:
    return _read(path("SOUL.md"))[:SOUL_CAP] or "You are Deadlee, a friendly helper."


def family_text() -> str:
    return _read(path("FAMILY.md"))


def person_file(name: str) -> str:
    return path("people", re.sub(r"[^a-z0-9-]", "", name.lower())[:20] + ".md")


def aliases() -> dict[str, str]:
    """'dad' -> 'alex' from People lines like 'Alex (Dad)'; existing profile files count too."""
    out = {n: n for n in people()}
    out.setdefault(MAIN_PERSON, MAIN_PERSON)
    for line in family.parse(family_text()).people:
        words = re.findall(r"[A-Za-z][A-Za-z'-]+", line)
        if words:
            first = words[0].lower()
            for w in words:
                out[w.lower()] = first
    return out


def resolve_person(name: str) -> str | None:
    return aliases().get(name.strip().lower())


def person_cap(name: str) -> int:
    return MAIN_PERSON_CAP if name.lower() == MAIN_PERSON else PERSON_CAP


def people() -> list[str]:
    try:
        return sorted(f[:-3] for f in os.listdir(path("people")) if f.endswith(".md"))
    except OSError:
        return []


def front_matter(skill: str) -> dict[str, str]:
    """The settings at the top of a skill file (the "key: value" lines between --- marks)."""
    m = re.match(r"---(.*?)---", _read(path("skills", skill + ".md")), flags=re.S)
    out = {}
    for line in (m.group(1).splitlines() if m else []):
        k, sep, v = line.partition(":")
        if sep and not k.strip().startswith("#"):
            out[k.strip()] = v.split("#")[0].strip()
    return out


def skills() -> str:
    d = path("skills")
    try:
        files = sorted(f for f in os.listdir(d) if f.endswith(".md") and f != "catalog.md")
    except OSError:
        return ""
    notes = []
    for f in files:
        body = re.sub(r"^---.*?---\s*", "", _read(os.path.join(d, f)), flags=re.S).strip()
        if body:
            notes.append(body)
    return ("## How to use your tools\n" + "\n".join(notes)) if notes else ""


@dataclass
class Snapshot:
    """What Deadlee knows, frozen at the start of a conversation."""
    soul: str
    knowledge: str
    skills: str
    family: family.Family
    taken: float = field(default_factory=time.time)


def snapshot(speaker: str | None = None) -> Snapshot:
    fam = family.parse(family_text())
    today = datetime.now(TZ).date()
    parts = [family.view(fam, today, speaker)]
    for name in people():
        text = _read(person_file(name)).strip()
        if text:
            if speaker and name == speaker.lower():
                parts.append(f"## About {name.title()} (who is talking to you now)\n{text}")
            else:
                parts.append(f"## About {name.title()}\n{text}")
    import calendar_sync                    # here, not at the top: it imports this module
    cal = calendar_sync.view()
    if cal:
        parts.insert(1, cal)
    mem = _read(path("MEMORY.md")).strip()
    if mem:
        parts.append("## Your own notes from earlier chats (use them naturally, don't list them)\n" + mem)
    return Snapshot(soul=soul(), knowledge="\n\n".join(parts), skills=skills(), family=fam)


# ---------------------------------------------------------------- memory tool

def _entries(text: str) -> list[str]:
    return [l[2:].strip() for l in text.splitlines() if l.startswith("- ")]


def _notes_split(text: str) -> tuple[str, list[str]]:
    """A person file is the parent's part, then '## Deadlee's notes' with Deadlee's entries."""
    head, sep, tail = text.partition(NOTES)
    return head.rstrip(), _entries(tail) if sep else []


def _render_person(head: str, notes: list[str]) -> str:
    return (head + "\n\n" if head else "") + NOTES + "\n" + "".join(f"- {n}\n" for n in notes)


def memory_op(action: str, target: str, content: str = "", old_text: str = "") -> str:
    """Hermes semantics: add / replace / remove. Over the cap is an error, so the
    model has to tidy up. Deadlee can only change its own entries: MEMORY.md and
    the "Deadlee's notes" part of a person file. Returns a short result for the model."""
    action, content, old_text = action.lower().strip(), " ".join(content.split()), old_text.strip()
    if action not in ("add", "replace", "remove"):
        return "error: action must be add, replace or remove"
    if action in ("add", "replace") and not content:
        return "error: content is empty"
    if len(content) > 200:
        return "error: keep each note under 200 characters"
    who = target.strip().lower()
    with _lock:
        if who in ("", "memory", "me", "deadlee"):
            p, cap = path("MEMORY.md"), MEMORY_CAP          # entries only, one "- " line each
            notes = _entries(_read(p))
            render = (lambda n: "".join(f"- {x}\n" for x in n))
        else:
            who = resolve_person(who)
            if not who:
                return ("error: that isn't someone in the family list. Use target 'memory' "
                        "for anything else.")
            p, cap = person_file(who), person_cap(who)
            head, notes = _notes_split(_read(p))
            render = (lambda n: _render_person(head, n))
        if action == "add":
            if any(content.lower() == n.lower() for n in notes):
                return "ok (already noted)"
            new = notes + [content]
        else:
            hits = [i for i, n in enumerate(notes) if old_text and old_text.lower() in n.lower()]
            if len(hits) != 1:
                return f"error: old_text matched {len(hits)} of your notes; give a unique piece of the note"
            new = list(notes)
            if action == "replace":
                new[hits[0]] = content
            else:
                del new[hits[0]]
        text = render(new)
        if len(text) > cap:
            return (f"error: that would make the notes {len(text)} of {cap} characters. "
                    "Replace or remove an old note first, then try again.")
        _write(p, text)
        return f"ok ({len(text)}/{cap} characters used)"


def replace_notes(target: str, notes: list[str]) -> bool:
    """Nightly tidy: swap a whole notes list (Deadlee's entries only) if it fits the cap."""
    with _lock:
        if target == "memory":
            p, cap, text = path("MEMORY.md"), MEMORY_CAP, "".join(f"- {n}\n" for n in notes)
        else:
            p, cap = person_file(target), person_cap(target)
            head, _ = _notes_split(_read(p))
            text = _render_person(head, notes)
        if len(text) > cap:
            return False
        _write(p, text)
        return True


def family_add(line: str) -> str:
    """Voice can only add to FAMILY.md, under '## Added by Deadlee'."""
    line = " ".join(line.split())[:160]
    if not line:
        return "error: nothing to add"
    with _lock:
        text = family_text()
        new = family.append_added(text, line, datetime.now(TZ).date())
        if len(new) > FAMILY_CAP:
            return "error: the family file is full; ask a grown-up to tidy it"
        _write(path("FAMILY.md"), new)
    return "ok (added to the family notes for a grown-up to check)"


def forget() -> None:
    """Setup -> FORGET MEMORY: Deadlee's own notes go. the parent's parts and FAMILY.md stay."""
    with _lock:
        _write(path("MEMORY.md"), "")
        for name in people():
            head, _ = _notes_split(_read(person_file(name)))
            _write(person_file(name), _render_person(head, []) if head else "")


def dump() -> dict:
    files = {"SOUL.md": soul(), "MEMORY.md": _read(path("MEMORY.md")), "FAMILY.md": family_text()}
    for name in people():
        files[f"people/{name}.md"] = _read(person_file(name))
    return files


# ---------------------------------------------------------------- editing from the web page

_EDITABLE = re.compile(r"(SOUL|FAMILY|MEMORY)\.md|(people|skills)/[a-z0-9-]{1,20}\.md")


def file_cap(rel: str) -> int:
    """Most characters a brain file may hold, or 0 if the page may not touch it."""
    if not _EDITABLE.fullmatch(rel):
        return 0
    if rel.startswith("skills/"):
        return CATALOG_CAP if rel == "skills/catalog.md" else SKILL_CAP
    if rel.startswith("people/"):
        return person_cap(rel[7:-3])
    return {"SOUL.md": SOUL_CAP, "FAMILY.md": FAMILY_CAP, "MEMORY.md": MEMORY_CAP}[rel]


def list_files() -> list[dict]:
    rels = ["SOUL.md", "FAMILY.md", "MEMORY.md"] + [f"people/{n}.md" for n in people()]
    try:
        rels += sorted(f"skills/{f}" for f in os.listdir(path("skills")) if file_cap(f"skills/{f}"))
    except OSError:
        pass
    return [{"path": r, "size": len(_read(path(*r.split("/")))), "cap": file_cap(r)} for r in rels]


def read_file(rel: str) -> str | None:
    return _read(path(*rel.split("/"))) if file_cap(rel) else None


def write_file(rel: str, text: str) -> str:
    """Save a whole file from the web page. Returns "ok ..." or "error: ..."."""
    cap = file_cap(rel)
    if not cap:
        return "error: not a brain file (use SOUL.md, FAMILY.md, MEMORY.md, people/<name>.md or skills/<name>.md)"
    text = text.replace("\r\n", "\n")
    if len(text) > cap:
        return f"error: that is {len(text)} of {cap} characters. Shorten it and save again."
    with _lock:
        _write(path(*rel.split("/")), text)
    return f"ok ({len(text)}/{cap} characters used)"


def delete_file(rel: str) -> str:
    """Only skills can be deleted (a skill that ships in brain-template comes back on restart)."""
    if not (file_cap(rel) and rel.startswith("skills/")):
        return "error: only skills can be deleted"
    try:
        os.remove(path(*rel.split("/")))
    except OSError:
        return "error: no such skill"
    return "ok"


# ---------------------------------------------------------------- gaps + events

def _append_jsonl(p: str, rec: dict, keep: int) -> None:
    with _lock:
        lines = _read(p).splitlines()[-(keep - 1):]
        lines.append(json.dumps(rec, ensure_ascii=True))
        _write(p, "\n".join(lines) + "\n")


def redact(text: str, names: list[str]) -> str:
    """Strip numbers and family names so the gap log holds what was asked, not who or where."""
    text = re.sub(r"\d[\d\s\-:/.]*", "#", text)
    for n in names:
        text = re.sub(rf"\b{re.escape(n)}\b", "[name]", text, flags=re.I)
    return " ".join(text.split())[:200]


def log_gap(request: str, why: str, would_need: str, names: list[str]) -> None:
    _append_jsonl(path("gaps.jsonl"), {"t": int(time.time()), "request": redact(request, names),
                                       "why": redact(why, names)[:120], "would_need": would_need[:120]},
                  GAPS_MAX)


def log_event(kind: str, detail: str = "") -> None:
    _append_jsonl(path("events.jsonl"), {"t": int(time.time()), "kind": kind, "detail": detail[:160]},
                  EVENTS_MAX)


def read_jsonl(name: str, since: float = 0) -> list[dict]:
    out = []
    for line in _read(path(name)).splitlines():
        try:
            rec = json.loads(line)
        except ValueError:
            continue
        if rec.get("t", 0) >= since:
            out.append(rec)
    return out
