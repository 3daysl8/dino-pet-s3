"""Which of Deadlee's modes a sentence belongs to: the bookshelf, the story maker or the
maths game. Decided here, in code, before the chat model sees anything.

The rule that matters: asking clearly for something else always gets you out of a story or
a game. (The first version treated everything said during a story as the next plot idea,
so "read me Winnie the Pooh" became part of the story and there was no way out.)

handle() returns a turn for app.py to speak, or None (not ours: let the chat model answer).
Every turn has "reply" (the bubble line) and "speak" (the words said aloud), maybe "speed",
and one of "story", "maths" or "page" when it is a story scene (stories.py), a maths turn
(maths.py) or a book page (books.py). A plain line from Deadlee has none of those.
"""
import re
import time

import books
import maths
import prompt
import stories

ASKED_S = 90                        # how long "which one?" waits for an answer
CHOICE = "Shall I read you one from the bookshelf, or shall we make one up together?"
MAKE = re.compile(r"\b(?:make|made|invent|create|together|our own|new one)\b", re.I)
SHELF = re.compile(r"\b(?:read|books?|bookshelf|shelf|library)\b", re.I)

_shelf_at = 0.0                     # Deadlee just listed books: a title on its own picks one
_choice_at = 0.0                    # Deadlee just asked "read one, or make one up?"


def _say(text: str) -> dict:
    return {"reply": prompt.tidy(text), "speak": text}


def handle(heard: str, by: str = "", reading: bool = False) -> dict | None:
    """reading: a book page was read a moment ago, so "keep going" means the next page."""
    global _shelf_at, _choice_at
    now = time.time()
    want = books.wants_read(heard)
    idea = stories.wants_story(heard)
    vague = stories.vague(heard)
    asks_maths = maths.wants_maths(heard)
    in_story = stories.active() or stories.waiting_for_idea()
    game = maths.active()

    # Asking Deadlee for something else gets out of the story (it is kept; "let's finish our
    # story" picks it up). "We read a magic book" is a plot idea, so only a direct ask counts.
    if in_story and ((want and want["direct"]) or asks_maths):
        stories.pause()
        in_story = None
    if game and (want or vague or idea is not None):    # ...or out of the game
        maths.stop()
        game = None
    if in_story:
        return stories.stop() if stories.wants_stop(heard) else stories.turn(heard)
    if game:
        return maths.answer(heard)

    asked_shelf, asked_choice = now - _shelf_at < ASKED_S, now - _choice_at < ASKED_S
    _shelf_at = _choice_at = 0.0
    if want and want.get("book"):                       # "read me Winnie the Pooh"
        return books.turn(books.read(want["book"]["title"]))
    if want:                                            # "read me a story": which one?
        _shelf_at = now
        return _say(books.shelf_line(want))
    if (asked_shelf or asked_choice) and len(heard.split()) <= 8:
        book = books.find(heard)                        # just the title, after "which one?"
        if book:
            return books.turn(books.read(book["title"]))
    if asked_choice and MAKE.search(heard):
        return stories.start("", by)
    if asked_choice and SHELF.search(heard):
        _shelf_at = now
        return _say(books.shelf_line({"titles": [b["title"] for b in books.library()], "by": ""}))
    if idea is not None:                                # "let's make a story about..."
        return stories.start(idea, by)
    if vague:                                           # "tell me a story": read one or make one?
        _choice_at = now
        return _say(CHOICE)
    if asks_maths:
        return maths.start(by)
    if reading and books.wants_more(heard):             # "keep going": the next page
        page = books.read()
        return books.turn(page) if page else None
    if reading and books.wants_stop(heard):             # "stop" / "no": not the chat model's to guess
        return _say(books.stop_line())
    return None
