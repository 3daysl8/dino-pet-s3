"""The bookshelf: books Deadlee can read aloud, a page at a time.

    brain/books/<name>.txt        one book: "Title: ...", "Author: ...", a blank line, then the
                                  text with a blank line between paragraphs
    brain/books/bookmarks.json    the next page to read, per book

A page is about a minute of reading and always ends on a paragraph. The text is spoken
as written: it never goes through the model, so the pace is set by pace() (speed and
pauses, from the top of brain/skills/books.md), not by a prompt. Books live with the brain (so the nightly
backup has them) and are never in git.
"""
import difflib
import json
import os
import re
import time

import httpx

import brain

PAGE_WORDS = 170                    # about a minute aloud
LONG_PARAGRAPH = 260                # split a paragraph longer than this at sentence ends
BOOK_MAX = 600_000                  # characters
GUTENBERG = "https://www.gutenberg.org/cache/epub/{id}/pg{id}.txt"


def _dir() -> str:
    return brain.path("books")


def slug(title: str) -> str:
    return re.sub(r"[^a-z0-9]+", "-", title.lower()).strip("-")[:40] or "book"


def _file(name: str) -> str:
    return os.path.join(_dir(), name + ".txt")


def _parse(raw: str) -> tuple[str, str, str]:
    head, _, body = raw.replace("\r\n", "\n").partition("\n\n")
    title = re.search(r"^Title:\s*(.+)$", head, re.M)
    author = re.search(r"^Author:\s*(.+)$", head, re.M)
    if not title:                                       # no header: the whole file is the text
        return "", "", raw
    return title.group(1).strip(), author.group(1).strip() if author else "", body


def paginate(text: str) -> list[str]:
    paras = [" ".join(p.split()) for p in re.split(r"\n\s*\n", text) if p.strip()]
    pieces = []
    for p in paras:
        if len(p.split()) <= LONG_PARAGRAPH:
            pieces.append(p)
            continue
        part = ""
        for sentence in re.split(r"(?<=[.!?])\s+", p):
            if part and len((part + " " + sentence).split()) > PAGE_WORDS:
                pieces.append(part)
                part = sentence
            else:
                part = (part + " " + sentence).strip()
        if part:
            pieces.append(part)
    pages, page = [], []
    for p in pieces:
        chapter = re.match(r"(chapter|part|book)\s+[0-9ivxlc]+\b", p, re.I) and len(p.split()) < 25
        words = sum(len(x.split()) for x in page)
        if page and (chapter or words + len(p.split()) > PAGE_WORDS * 1.25 or words >= PAGE_WORDS):
            pages.append(page)
            page = []
        page.append(p)
    if page:
        pages.append(page)
    return ["\n\n".join(p) for p in pages]


def _bookmarks() -> dict:
    try:
        return json.load(open(os.path.join(_dir(), "bookmarks.json"), encoding="utf-8"))
    except (OSError, ValueError):
        return {}


def _save_bookmarks(marks: dict) -> None:
    brain._write(os.path.join(_dir(), "bookmarks.json"), json.dumps(marks))


def _load(name: str) -> dict | None:
    raw = brain._read(_file(name))
    if not raw.strip():
        return None
    title, author, body = _parse(raw)
    return {"name": name, "title": title or name.replace("-", " ").title(), "author": author,
            "pages": paginate(body)}


def library() -> list[dict]:
    """Every book: name, title, author, how many pages, and the page it is up to (1-based)."""
    try:
        names = sorted(f[:-4] for f in os.listdir(_dir()) if f.endswith(".txt"))
    except OSError:
        return []
    marks, out = _bookmarks(), []
    for name in names:
        book = _load(name)
        if book and book["pages"]:
            mark = marks.get(name, {})
            out.append({"name": name, "title": book["title"], "author": book["author"],
                        "pages": len(book["pages"]), "next_page": min(mark.get("page", 0), len(book["pages"]) - 1) + 1,
                        "last_read": mark.get("at", 0)})
    return out


FILLER = set("the a an of tale story storie book me us you i to from library bookshelf shelf some another "
             "different one read want would like can could please what which do we have got there is are and "
             "by with for in on it that this new great little big s".split())


def _plain(s: str) -> str:
    """The words that tell books apart: lower case, no filler, no plural or possessive s."""
    words = re.findall(r"[a-z0-9]+", s.lower())
    words = [w[:-1] if len(w) > 3 and w.endswith("s") and not w.endswith("ss") else w for w in words]
    return " ".join(w for w in words if w not in FILLER)


def find(title: str) -> dict | None:
    """The book someone means: 'peter rabbit' finds 'The Tale of Peter Rabbit'."""
    want = _plain(title)
    shelf = library()
    if not want or not shelf:
        return None
    for b in shelf:
        have = _plain(b["title"])
        if want == have or want in have or have in want:
            return b
    shared = [b for b in shelf if set(want.split()) & set(_plain(b["title"]).split())]
    if len(shared) == 1:                                # "pooh bear": one book has a word of it
        return shared[0]
    names = {_plain(b["title"]): b for b in shelf}
    close = difflib.get_close_matches(want, list(names), n=1, cutoff=0.6)
    return names[close[0]] if close else None


MOVING = re.compile(r"\b(?:page|chapter|again|back|skip|beginning|start|keep|more|next|carry on|continue)\b", re.I)
READ = re.compile(r"\bread\b(?:\s+(?:to|me|us|you|it|out|aloud))*\s*(?P<what>[^.?!]*)", re.I)
DIRECT = re.compile(r"\bread\s+(?:to\s+)?(?:me|us)\b|\b(?:can|could|will|would) you\b.{0,12}\bread\b|"
                    r"\bwant (?:you )?to\b.{0,12}\bread\b|\bread\b.{0,30}\b(?:please|bookshelf|library)\b", re.I)
SHELF_ASK = re.compile(r"\b(?:what|which)\b.{0,20}\b(?:books?|stories)\b.{0,25}\b(?:have|got|there|shelf|library|read)\b", re.I)
GENERIC = re.compile(r"\bread\b\s+(?:\w+\s+){0,4}?(?:me|us|a|an|the|one|something|another|story|book)\b", re.I)
STORYISH = re.compile(r"\b(?:stor(?:y|ies)|fairy ?tales?|tales?|books?|one)\b", re.I)


def _author_in(heard: str) -> str:
    """An author on the shelf who is named in what was said ('Anderson' finds Andersen)."""
    said = [w for w in re.findall(r"[a-z]+", heard.lower()) if len(w) >= 5]
    for author in dict.fromkeys(b["author"] for b in library() if b["author"]):
        last = re.findall(r"[a-z]+", author.lower())[-1]
        if last != "deadlee" and difflib.get_close_matches(last, said, n=1, cutoff=0.8):
            return author
    return ""


def wants_read(heard: str) -> dict | None:
    """Is this asking for a book to be read? {"book": entry} for a title on the shelf, or
    {"titles": [...], "by": author} when it doesn't say which ("read me a story", "one by
    Hans Christian Andersen", "what books do we have?"). "direct" is True when Deadlee is
    plainly being asked ("read me...", "can you read..."). None if it isn't, or if it is
    about moving within a book (pages, chapters, again: the read_book tool does those)."""
    asks = bool(SHELF_ASK.search(heard))
    m = READ.search(heard)
    author = _author_in(heard)
    if MOVING.search(heard) or not (m or asks or (author and STORYISH.search(heard))):
        return None
    direct = asks or bool(author) or bool(DIRECT.search(heard))
    if m and _plain(m.group("what")):
        book = find(m.group("what"))
        if book and not (author and _plain(book["author"]) and book["author"] != author):
            return {"book": book, "direct": True}
    if author:
        return {"titles": [b["title"] for b in library() if b["author"] == author], "by": author, "direct": True}
    if asks or GENERIC.search(heard):
        return {"titles": [b["title"] for b in library()], "by": "", "direct": direct}
    return None


def shelf_line(want: dict) -> str:
    """Deadlee saying which books there are, then asking which one."""
    titles = want["titles"]
    if not titles:
        return "There are no books on the shelf yet. A grown-up can add some."
    shown = titles[:8]
    listed = ", ".join(shown[:-1]) + (" or " if len(shown) > 1 else "") + shown[-1]
    if len(titles) > len(shown):
        listed = ", ".join(shown) + f", and {len(titles) - len(shown)} more"
    if want.get("by"):
        return f"I have {len(titles)} by {want['by']}: {listed}. Which one would you like?"
    return f"I can read you {listed}. Which one would you like?"


def last_read() -> dict | None:
    shelf = [b for b in library() if b["last_read"]]
    return max(shelf, key=lambda b: b["last_read"]) if shelf else None


def shelf_text() -> str:
    shelf = library()
    if not shelf:
        return "There are no books on the shelf yet. A grown-up can add some."
    lines = []
    for b in shelf:
        where = "not started" if b["next_page"] == 1 and not b["last_read"] else f"up to page {b['next_page']} of {b['pages']}"
        lines.append(f"- {b['title']}" + (f" by {b['author']}" if b["author"] else "") + f" ({where})")
    return "Books on the shelf:\n" + "\n".join(lines)


WHERE = ("bookmark", "start", "again", "back", "forward")


def chapters(pages: list[str]) -> dict[int, int]:
    """Chapter number -> the page (0-based) it starts on, from "Chapter 4" / "CHAPTER IV" headings."""
    out = {}
    for i, text in enumerate(pages):
        m = re.match(r"chapter\s+(\d+|[ivxlc]+)\b", text, re.I)
        if m:
            word = m.group(1).upper()
            n = int(word) if word.isdigit() else int(_heading("CHAPTER " + word).split()[1])
            out.setdefault(n, i)
    return out


def read(title: str = "", where: str = "bookmark", page: int = 0, chapter: int = 0) -> dict | None:
    """A page of a book (the last one read if no title), moving its bookmark to the page after.
    where: bookmark = carry on, start = from the beginning, again = the page just read,
    back = the one before that, forward = skip one. page / chapter (1-based) jump straight there.
    Returns {title, author, page, pages, text, first, last, jumped}, or None if there is no
    such book. Raises ValueError (with words for the listener) for a page or chapter that isn't there."""
    entry = find(title) if title.strip() else last_read()
    book = _load(entry["name"]) if entry else None
    if not book:
        return None
    total, marks = len(book["pages"]), _bookmarks()
    mark = marks.get(book["name"], {})
    nxt = min(mark.get("page", 0), total - 1)             # the page a plain "keep going" would read
    if chapter:
        starts = chapters(book["pages"])
        if chapter not in starts:
            raise ValueError(f"{book['title']} has no chapter {chapter}. " + (
                f"It has chapters {min(starts)} to {max(starts)}." if starts else "It has no chapters, only pages."))
        n = starts[chapter]
    elif page:
        if not 1 <= page <= total:
            raise ValueError(f"{book['title']} has pages 1 to {total}.")
        n = page - 1
    else:
        # A finished book's bookmark is back at page 1, so "again" then means the last page.
        just_read = (nxt - 1) % total if mark else 0
        n = {"start": 0, "again": just_read, "back": max(just_read - 1, 0),
             "forward": min(nxt + 1, total - 1)}.get(where, nxt)
    last = n == total - 1
    marks[book["name"]] = {"page": 0 if last else n + 1, "at": time.time()}   # finished: back to the start
    _save_bookmarks(marks)
    return {"title": book["title"], "author": book["author"], "page": n + 1, "pages": total,
            "text": book["pages"][n], "first": n == 0, "last": last,
            "jumped": bool(chapter or page or where in ("again", "back", "forward"))}


def set_bookmark(name: str, page: int) -> str:
    """From the web page: the next page to read (1-based)."""
    book = _load(name) if re.fullmatch(r"[a-z0-9-]{1,40}", name) else None
    if not book:
        return "error: no such book"
    if not 1 <= page <= len(book["pages"]):
        return f"error: that book has pages 1 to {len(book['pages'])}"
    marks = _bookmarks()
    marks[name] = {"page": page - 1, "at": marks.get(name, {}).get("at", 0)}
    _save_bookmarks(marks)
    return "ok"


KEEP_GOING = re.compile(
    r"(?:(?:yes|yeah|yep|ok|okay|sure)(?: please)?|keep (?:going|reading)|(?:read )?(?:some )?more|"
    r"next(?: page)?|another page|go on|continue|read on|(?:and )?then what(?: happened)?)(?: please)?")


def wants_more(heard: str) -> bool:
    """Right after a page: is this just 'keep going'? (Then the next page needs no model.)"""
    return bool(KEEP_GOING.fullmatch(" ".join(re.sub(r"[^a-z ]", " ", heard.lower()).split())))


STOP = re.compile(r"\b(?:stop|enough|no more|not now|that s all)\b|^(?:no|nope)(?: thanks?| thank you)?$")
NOT_THE_BOOK = re.compile(r"\b(?:timer|alarm|radio|music|song|light)\b")


def wants_stop(heard: str) -> bool:
    """Right after a page: is this 'stop' / 'no' (to "Shall I keep going?")? Not if it names
    something else to stop, like the timer or the radio."""
    flat = " ".join(re.sub(r"[^a-z ]", " ", heard.lower()).split())
    return bool(STOP.search(flat)) and not NOT_THE_BOOK.search(flat)


def stop_line() -> str:
    book = last_read()
    return f"Okay, I'll stop there. I've kept our place in {book['title']} for next time." if book \
        else "Okay, I'll stop there."


PACE = {"speed": (0.9, 0.6, 1.3), "pause": (0.8, 0.0, 5.0), "sentence_pause": (0.0, 0.0, 2.0)}   # default, min, max


def pace() -> dict:
    """How books are read, from the top of brain/skills/books.md (edit it on the web page):
        ---
        speed: 0.9            1.0 is the normal talking speed; lower is slower
        pause: 0.8            seconds of quiet between paragraphs
        sentence_pause: 0     extra seconds between sentences
        ---
    Anything missing or out of range falls back to the default."""
    out = {k: v[0] for k, v in PACE.items()}
    wanted = brain.front_matter("books")
    for k, (_, lo, hi) in PACE.items():
        try:
            out[k] = min(max(float(wanted[k]), lo), hi)
        except (KeyError, ValueError):
            pass
    return out


def _quiet(seconds: float) -> str:
    """The voice engine's pause mark (Kokoro: "[pause:0.8s]")."""
    return f" [pause:{seconds:g}s] " if seconds > 0 else ""


def spoken(page: dict, how: dict | None = None) -> str:
    """What is read aloud for a page: the paragraphs, with a pause between them."""
    how = how or pace()
    parts = [" ".join(p.split()) for p in page["text"].split("\n\n")]
    if how["sentence_pause"] > 0:
        parts = [re.sub(r"(?<=[.!?])\s+(?=[A-Z\"'])", _quiet(how["sentence_pause"]), p) for p in parts]
    if page["first"]:
        parts.insert(0, page["title"] + (f", by {page['author']}" if page["author"] else "") + ".")
    elif page.get("jumped"):                              # say where we landed
        parts.insert(0, f"Page {page['page']}.")
    parts.append("The end." if page["last"] else "Shall I keep going?")
    return (_quiet(how["pause"]) or "\n\n").join(parts)


def bubble(page: dict) -> str:
    """The short line for the toy's speech bubble."""
    return f"{page['title']}: page {page['page']} of {page['pages']}." + (" The end!" if page["last"] else "")


def turn(page: dict) -> dict:
    """A page as a turn for app.py, the same shape as a story scene or a maths turn: the
    bubble line, the words to speak and how fast, plus the page itself."""
    how = pace()
    return {"reply": bubble(page), "speak": spoken(page, how), "speed": how["speed"], "page": page}


# ---------------------------------------------------------------- adding and removing

def add(title: str, author: str, text: str) -> str:
    title, author = " ".join(title.split())[:80], " ".join(author.split())[:60]
    text = text.replace("\r\n", "\n").strip()
    if not title or len(text.split()) < 20:
        return "error: a book needs a title and some text"
    if len(text) > BOOK_MAX:
        return f"error: that is {len(text)} characters; the most a book can be is {BOOK_MAX}"
    brain._write(_file(slug(title)), f"Title: {title}\nAuthor: {author}\n\n{text}\n")
    return f"ok ({len(paginate(text))} pages)"


def remove(name: str) -> str:
    if not re.fullmatch(r"[a-z0-9-]{1,40}", name):
        return "error: no such book"
    try:
        os.remove(_file(name))
    except OSError:
        return "error: no such book"
    marks = _bookmarks()
    if marks.pop(name, None) is not None:
        _save_bookmarks(marks)
    return "ok"


def _heading(para: str) -> str:
    """Headings in capitals read badly aloud: 'CHAPTER IV' -> 'Chapter 4', 'IN WHICH...' -> 'In which...'."""
    flat = " ".join(para.split())
    chapter = re.fullmatch(r"CHAPTER ([IVXLC]+)\.?", flat)
    if chapter:
        total, prev = 0, 0
        for ch in reversed(chapter.group(1)):
            v = {"I": 1, "V": 5, "X": 10, "L": 50, "C": 100}[ch]
            total, prev = (total - v, prev) if v < prev else (total + v, v)
        return f"Chapter {total}"
    letters = [c for c in flat if c.isalpha()]
    if len(flat.split()) >= 3 and letters and all(c.isupper() for c in letters):
        return flat.capitalize()
    return para


def clean_gutenberg(raw: str, start: str = "") -> tuple[str, str, str]:
    """Title, author and the story from a Project Gutenberg plain-text file: no licence
    header or footer, no [Illustration] notes. start: skip everything before these words."""
    raw = raw.replace("\r\n", "\n").lstrip("﻿")
    title = re.search(r"^Title:\s*(.+)$", raw, re.M)
    author = re.search(r"^Author:\s*(.+)$", raw, re.M)
    # after the last START line: some files repeat the header under the first one
    body = re.split(r"\*\*\* ?START OF TH[EI]S? PROJECT GUTENBERG EBOOK[^\n]*\*\*\*", raw)[-1]
    body = re.split(r"\*\*\* ?END OF TH[EI]S? PROJECT GUTENBERG EBOOK", body, maxsplit=1)[0]
    body = re.sub(r"\[(?:Illustration|Transcriber)[^\]]*\]", "", body)
    body = re.sub(r"^[ \t]*(?:[_*=-][ \t]*){3,}$", "", body, flags=re.M)     # rules and "*   *   *"
    body = re.sub(r"_([^_\n]+)_", r"\1", body)                        # _italics_
    if start.strip():
        at = body.lower().find(" ".join(start.split()).lower())
        if at >= 0:
            body = body[at:]
    body = re.sub(r"\n[ \t]+\n", "\n\n", body)
    body = re.sub(r"\n{3,}", "\n\n", body).strip()
    body = re.sub(r"\s*\bTHE END\.?$", "", body)                       # Deadlee says it itself
    body = "\n\n".join(_heading(p) for p in body.split("\n\n"))
    return (title.group(1).strip() if title else "", author.group(1).strip() if author else "", body)


def add_gutenberg(book_id: int, start: str = "") -> str:
    r = httpx.get(GUTENBERG.format(id=int(book_id)), timeout=30.0, follow_redirects=True,
                  headers={"User-Agent": "pet-bridge/1.0 (home family assistant)"})
    if r.status_code == 404:
        return "error: Project Gutenberg has no plain-text book with that number"
    r.raise_for_status()
    title, author, body = clean_gutenberg(r.content.decode("utf-8", errors="replace"), start)
    if not title:
        return "error: that file has no title"
    return add(title, author, body)
