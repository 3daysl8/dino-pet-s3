"""Story maker: the child and Deadlee make up a story together, one scene at a time.

    brain/stories/<id>.json     one story: the idea, the cast, every scene (see new_story)
    brain/stories/active.json   which story is being told right now (so a restart can't lose it)
    brain/stories/world.json    friends met in finished stories, who can come back in new ones
    brain/skills/stories.md     settings at the top: scenes, per_day, not_after

Who decides what:
    This module decides how many scenes there are, which beat comes next (opening, middle,
    turning point, ending), when the story ends, who is in the cast, what is saved and
    whether a scene is acceptable.
    The model writes the words: narration, a question, two ideas, a mood, and proposals
    (a title, a new friend, the place, a short summary). Proposals are checked here first.
The model only ever sees a compact brief for the next scene, never a conversation.
Every scene is checked; a bad one is asked for once more and then replaced by a
hand-written scene, so a story can always reach its ending.
A finished story goes on the bookshelf (books.py), so it can be read back later.
"""
import json
import logging
import os
import re
import time
from datetime import datetime

import books
import brain
import prompt

log = logging.getLogger("pet-bridge")

RETRY_WITHIN_S = 8                  # a second try only if the first was quick: the toy waits 30 s for a reply
ACTIVE_S = 10 * 60                  # a story left alone this long has to be picked up again by asking
SETTINGS = {"scenes": (5, 4, 8), "per_day": (3, 1, 20)}        # default, min, max
NOT_AFTER = "19:30"                 # no new story after this time (HH:MM; "" = any time)
CAST_MAX = 8
FRIENDS_MAX = 24                    # friends remembered between stories (the longest unseen drop off)
FRIENDS_SHOWN = 5                   # how many of them the model is told about for a new scene
MOODS = ["happy", "curious", "surprised", "proud", "sleepy"]

SCHEMA = {"type": "object", "additionalProperties": False,
          "required": ["narration", "question", "idea_a", "idea_b", "mood", "title", "new_friend", "place", "summary"],
          "properties": {"narration": {"type": "string"}, "question": {"type": "string"},
                         "idea_a": {"type": "string"}, "idea_b": {"type": "string"},
                         "mood": {"type": "string", "enum": MOODS}, "title": {"type": "string"},
                         "new_friend": {"type": "string"}, "place": {"type": "string"},
                         "summary": {"type": "string"}}}

# Kept in git on purpose: change the wording here, not in the brain folder.
SYSTEM = """You are Deadlee, a friendly dinosaur helper, making up a story out loud together with {child}, \
who is 6. {child} and Deadlee are both in the story and go on the adventure together.

Write ONE scene at a time, as JSON:
- narration: 40 to 70 words, short sentences, plain words, said "we" or by name. It is spoken aloud.
- question: one short question asking {child} what happens next.
- idea_a, idea_b: two different things we could do, each 3 to 8 words, starting with a doing word.
- mood: how Deadlee feels in this scene.
- title: a short name for the whole story, 2 to 6 words.
- new_friend: the name of one new friend who joined in this scene (like "Zog the robot snail"), or "".
- place: where we are now, in a few words.
- summary: the whole story so far in one or two sentences.

Rules:
- When {child} says what happens, that is what happens. Use their idea, even if it is silly.
- Gentle and happy: nobody gets hurt, nothing frightening, no fighting, no weapons, nobody is mean.
- Only make-believe. Never ask about real things like where {child} lives, their school or their family.
- No roaring or growling noises, no sound effects, no emoji."""

BEATS = {
    "opening": "Begin the adventure in the place from the idea. Stop at a moment where something could happen.",
    "middle": "Make what {child} said happen straight away. Then add one small, friendly surprise.",
    "turning": "Make what {child} said happen. Then reach the big moment of the story: the problem is nearly solved.",
    "ending": "Make what {child} said happen and finish the story happily: the problem is solved, everyone is safe, "
              "and we go home. This is the last scene, so question, idea_a and idea_b must be empty.",
}

# Hand-written scenes for when the model can't be used. {child}, {idea} are filled in.
FALLBACK = {
    "opening": ("{child} and Deadlee set off on an adventure about {idea}. The sun was warm and the path was wide. "
                "Soon they came to a fork in the road, with a tall hill on one side and a quiet stream on the other.",
                "Which way shall we go?", "Climb the tall hill", "Follow the quiet stream", "curious"),
    "middle": [("So that is just what they did. Along the way they found a little wooden door in the side of a tree, "
                "with a tiny bell beside it and a note that said: friends welcome.",
                "What shall we do?", "Ring the tiny bell", "Peek through the keyhole", "surprised"),
               ("So that is just what they did. Around the corner was a wobbly bridge made of rope and planks, "
                "and on the far side something shiny was twinkling in the grass.",
                "How do we get across?", "Tiptoe over the bridge", "Look for another way round", "curious"),
               ("So that is just what they did. Then they heard a tiny voice calling hello. It was a small snail "
                "with a striped shell, who had lost the way home and wanted to come along.",
                "What shall we say?", "Invite the snail to join us", "Help the snail find home", "happy")],
    "turning": ("So that is just what they did. At last they found what they had been looking for, right in the "
                "middle of a sunny clearing. But it was sitting on a high rock, just out of reach.",
                "How do we get it down?", "Build a tower of logs", "Ask a friendly bird to help", "curious"),
    "ending": ("And it worked! Everyone cheered, and {child} and Deadlee shared a big picnic with all their new "
               "friends. When the stars came out they walked home together, tired and happy, already dreaming "
               "about their next adventure.", "", "", "", "proud"),
}

NOT_FOR_STORIES = re.compile(                       # whole words, then word beginnings ("warm" is fine)
    r"\b(?:wars?|hell|die[sd]?|dying|dead|guns?|stab(?:s|bed|bing)?|hate[sd]?|beers?|wines?|sexy?|crap|damn|"
    r"drugs?|shoot(?:s|ing)?|kill(?:s|ed|ing|ers?)?|bombs?|knife|knives|weapons?|blood|bloody|bleed(?:s|ing)?|"
    r"cigars?|cigarettes?|nude|naked|idiots?|stupid|devils?|demons?|zombies?|corpses?|drunk)\b|"
    r"\b(?:murder|poison|drown|strangl|suicid|terror|kidnap|death)\w*", re.I)
PERSONAL = re.compile(r"\b(where do you live|your (?:address|school|phone|last name|surname|password|teacher)|"
                      r"how old are you|what is your (?:real )?name)\b", re.I)
ROAR = re.compile(r"\b(?:r+a+w+r+|r+o+a+r+)\w*[!,.]*\s*", re.I)
A_STORY = r"(?:stor(?:y|ies)|adventures?|fairy ?tales?|tales?)"
MAKE_ONE = re.compile(rf"\b(?:make|made|invent|create|write)\b.{{0,30}}\b{A_STORY}\b", re.I)
ASK_ONE = re.compile(rf"\b(?:tell|want|have|give|hear|do|start|would like|'d like)\b.{{0,30}}\b{A_STORY}\b", re.I)
ABOUT = re.compile(rf"\b{A_STORY}\s+(?:about|where|with)\s+(?P<idea>.+)", re.I)
WANTS_STOP = re.compile(
    r"\b(?:stop|end|finish)\b.{0,12}\b(?:story|adventure)\b|"
    r"^\W*(?:the end|i'?m done|no more|stop|stop it|stop now|cancel|never ?mind|no thanks?|no thank you|"
    r"i don'?t want (?:a|to|this|that|any)\b.*)\W*$", re.I)


# ---------------------------------------------------------------- files and settings

def _dir() -> str:
    return brain.path("stories")


def _load(story_id: str) -> dict | None:
    if not re.fullmatch(r"[a-z0-9-]{1,60}", story_id or ""):
        return None
    try:
        return json.load(open(os.path.join(_dir(), story_id + ".json"), encoding="utf-8"))
    except (OSError, ValueError):
        return None


def _save(story: dict) -> None:
    brain._write(os.path.join(_dir(), story["id"] + ".json"), json.dumps(story, indent=1))


def _active_file() -> str:
    return os.path.join(_dir(), "active.json")


def _set_active(story_id: str | None, waiting_for_idea: bool = False, by: str = "") -> None:
    brain._write(_active_file(), json.dumps({"id": story_id, "waiting": waiting_for_idea, "by": by, "at": time.time()}))


def _mark() -> dict:
    try:
        mark = json.load(open(_active_file(), encoding="utf-8"))
    except (OSError, ValueError):
        return {}
    return mark if time.time() - mark.get("at", 0) < ACTIVE_S else {}


def active() -> dict | None:
    """The story being told right now, if it was touched in the last few minutes."""
    story = _load(_mark().get("id") or "")
    return story if story and not story["done"] else None


def waiting_for_idea() -> bool:
    """Deadlee has just asked what the story should be about."""
    return bool(_mark().get("waiting"))


def settings() -> dict:
    wanted = brain.front_matter("stories")
    out = {k: v[0] for k, v in SETTINGS.items()}
    for k, (_, lo, hi) in SETTINGS.items():
        try:
            out[k] = min(max(int(float(wanted[k])), lo), hi)
        except (KeyError, ValueError):
            pass
    cutoff = wanted.get("not_after", NOT_AFTER).strip()
    out["not_after"] = cutoff if re.fullmatch(r"\d{1,2}:\d{2}", cutoff) else ""
    return out


def library() -> list[dict]:
    """Every story, newest first, for the parent's page."""
    try:
        ids = [f[:-5] for f in os.listdir(_dir()) if f.endswith(".json") and f not in ("active.json", "world.json")]
    except OSError:
        return []
    out = []
    for story in filter(None, (_load(i) for i in ids)):
        out.append({"id": story["id"], "title": story["title"], "idea": story["idea"], "by": story["by"],
                    "scenes": len(story["scenes"]), "of": story["scenes_planned"], "done": story["done"],
                    "started": story["started"], "book": story.get("book", ""),
                    "text": "\n\n".join(s["narration"] for s in story["scenes"])})
    return sorted(out, key=lambda s: s["started"], reverse=True)


def remove(story_id: str) -> str:
    story = _load(story_id)
    if not story:
        return "error: no such story"
    os.remove(os.path.join(_dir(), story_id + ".json"))
    _forget_story(story_id)
    if story.get("book"):
        books.remove(story["book"])
    if _mark().get("id") == story_id:
        _set_active(None)
    return "ok"


# ---------------------------------------------------------------- friends who come back

def friends() -> list[dict]:
    """Friends met in finished stories, most recently seen first: {name, from, stories, seen}."""
    try:
        data = json.load(open(os.path.join(_dir(), "world.json"), encoding="utf-8"))
    except (OSError, ValueError):
        return []
    return sorted(data.get("friends", []), key=lambda f: f.get("seen", 0), reverse=True)


def _save_friends(items: list[dict]) -> None:
    brain._write(os.path.join(_dir(), "world.json"), json.dumps({"friends": items[:FRIENDS_MAX]}, indent=1))


def _known_name(name: str) -> str:
    """'Zip' or 'zip the snail' is our old friend 'Zip the robot snail'."""
    first = name.split()[0].lower() if name.split() else ""
    for f in friends():
        if len(first) >= 3 and f["name"].split()[0].lower() == first:
            return f["name"]
    return name


def _remember_friends(story: dict) -> None:
    """A finished story's friends (everyone but the child and Deadlee) can visit later ones."""
    known = {f["name"].lower(): f for f in friends()}
    for name in story["cast"][2:]:
        f = known.setdefault(name.lower(), {"name": name, "from": story["title"], "stories": [], "seen": 0})
        if story["id"] not in f["stories"]:
            f["stories"].append(story["id"])
        f["seen"] = time.time()
    _save_friends(sorted(known.values(), key=lambda f: f["seen"], reverse=True))


def _forget_story(story_id: str) -> None:
    """A deleted story takes with it the friends who were only ever in that one."""
    kept = []
    for f in friends():
        f["stories"] = [s for s in f["stories"] if s != story_id]
        if f["stories"]:
            kept.append(f)
    _save_friends(kept)


def remove_friend(name: str) -> str:
    kept = [f for f in friends() if f["name"].lower() != name.strip().lower()]
    if len(kept) == len(friends()):
        return "error: no such friend"
    _save_friends(kept)
    return "ok"


# ---------------------------------------------------------------- the rules

def wants_story(heard: str) -> str | None:
    """Asking to MAKE a story. 'Let's make a story about a jellybean planet' -> 'a jellybean
    planet'; 'let's make up a story' -> ''; None for anything else. Anything with 'read' in it
    is the bookshelf, and a bare 'tell me a story' is vague() (read one, or make one?)."""
    if re.search(r"\bread\b", heard, re.I):
        return None
    about = ABOUT.search(heard)
    idea = " ".join(about.group("idea").split()).strip(" .!?") if about else ""
    if MAKE_ONE.search(heard) or (idea and ASK_ONE.search(heard)):
        return idea
    return None


def vague(heard: str) -> bool:
    """'Tell me a story', 'can I have a fairy tale': a story is wanted, but read or made up?"""
    return not re.search(r"\bread\b", heard, re.I) and wants_story(heard) is None and bool(ASK_ONE.search(heard))


def pause() -> None:
    """Leave the story where it is (asked for something else). It stays unfinished and
    'let's finish our story' picks it up; nothing more said is taken as part of it."""
    _set_active(None)


def wants_stop(heard: str) -> bool:
    return bool(WANTS_STOP.search(heard))


def beat_for(n: int, total: int) -> str:
    return "opening" if n == 1 else "ending" if n >= total else "turning" if n == total - 1 else "middle"


def _clean(text: str, limit: int) -> str:
    text = text.replace("’", "'").replace("‘", "'").replace("“", '"').replace("”", '"')
    return " ".join(ROAR.sub("", text).split())[:limit].strip()


def check(scene: dict, beat: str) -> str | None:
    """Why this scene can't be used, or None if it is fine."""
    if not isinstance(scene, dict) or not all(isinstance(scene.get(k), str) for k in SCHEMA["required"]):
        return "wrong shape"
    words = len(scene["narration"].split())
    if not 25 <= words <= 110:
        return f"narration is {words} words"
    spoken = " ".join(scene[k] for k in ("narration", "question", "idea_a", "idea_b", "new_friend", "title"))
    bad = NOT_FOR_STORIES.search(spoken)
    if bad:
        return f"not suitable for a young child ({bad.group(0).lower()})"
    if PERSONAL.search(spoken):
        return "asks for something personal"
    if beat != "ending":
        a, b = scene["idea_a"].strip(" .!").lower(), scene["idea_b"].strip(" .!").lower()
        if not scene["question"].strip() or not a or not b or a == b:
            return "needs a question and two different ideas"
        if max(len(scene["idea_a"].split()), len(scene["idea_b"].split())) > 12 or len(scene["question"].split()) > 25:
            return "question or ideas too long"
    return None


def _fallback(story: dict, beat: str) -> dict:
    pick = FALLBACK[beat]
    if beat == "middle":
        pick = pick[len(story["scenes"]) % len(pick)]
    narration, question, a, b, mood = pick
    fill = {"child": story["by"].title(), "idea": story["idea"] or "a surprise"}
    return {"narration": narration.format(**fill), "question": question, "idea_a": a, "idea_b": b, "mood": mood,
            "title": "", "new_friend": "", "place": "", "summary": ""}


def brief(story: dict, beat: str, said: str) -> tuple[str, str]:
    """The compact system and user text for the next scene (nothing else goes to the model)."""
    child = story["by"].title()
    n = len(story["scenes"]) + 1
    lines = [f"Story idea: {story['idea'] or 'anything you like'}",
             "In the story: " + ", ".join(story["cast"])]
    old = [f for f in friends() if f["name"].lower() not in (c.lower() for c in story["cast"])][:FRIENDS_SHOWN]
    if old and beat != "ending":
        lines.append("Friends from our other stories, who may visit if it fits (give the exact name in new_friend): "
                     + "; ".join(f"{f['name']} (from {f['from']})" for f in old))
    if story["place"]:
        lines.append(f"Where we are: {story['place']}")
    if story["summary"]:
        lines.append(f"The story so far: {story['summary']}")
    if story["scenes"]:
        last = story["scenes"][-1]
        lines.append(f"Deadlee just asked: {last['question']} (ideas: {last['ideas'][0]} / {last['ideas'][1]})")
        lines.append(f'{child} said: "{said}"' if said else
                     f"{child} suggested something that doesn't suit this story. Don't mention it: "
                     "carry on with a friendlier idea of your own.")
    lines.append(f"Write scene {n} of {story['scenes_planned']}. " + BEATS[beat].format(child=child))
    return SYSTEM.format(child=child), "\n".join(lines)


def ask_model(system: str, user: str) -> dict:
    """One scene from the local model, held to SCHEMA. Tests replace this."""
    import llm                          # here, not at the top: llm imports tools, which imports this
    return llm.local_json(system, user, SCHEMA, max_tokens=420)


def _scene(story: dict, beat: str, said: str) -> tuple[dict, str]:
    """A checked scene and where it came from ('model', 'retry' or 'fallback')."""
    system, user = brief(story, beat, said)
    t0 = time.time()
    for attempt in ("model", "retry"):
        if attempt == "retry" and time.time() - t0 > RETRY_WITHIN_S:
            break
        try:
            scene = ask_model(system, user)
        except Exception as e:  # noqa: BLE001 - down, slow or not JSON: all mean "use the spare"
            log.warning("story scene: %s (%s)", type(e).__name__, attempt)
            brain.log_event("story_error", type(e).__name__)
            continue
        why = check(scene, beat)
        if not why:
            return scene, attempt
        log.info("story scene refused (%s): %s", attempt, why)
        brain.log_event("story_refused", why)
        word = re.search(r"\((\w+)\)$", why)             # tell the second try which word to leave out
        if word:
            user += f'\nDo not use the word "{word.group(1)}" or anything like it. Keep it gentle.'
    return _fallback(story, beat), "fallback"


# ---------------------------------------------------------------- telling a story

def new_story(idea: str, by: str, now: datetime | None = None) -> dict:
    now = now or datetime.now(prompt.TZ)
    stem = f"{now:%Y-%m-%d}-{books.slug(idea or 'story')[:30]}"
    story_id, n = stem, 2
    while _load(story_id):
        story_id, n = f"{stem}-{n}", n + 1
    child = (by or brain.MAIN_PERSON).lower()
    return {"id": story_id, "title": "", "idea": idea, "by": child, "started": time.time(),
            "scenes_planned": settings()["scenes"], "cast": [child.title(), "Deadlee"], "place": "",
            "summary": "", "scenes": [], "done": False, "book": ""}


def _may_start(now: datetime) -> str | None:
    """A kind 'not now' if today's limit or the evening cut-off says so."""
    rules = settings()
    today = [s for s in library() if s["id"].startswith(f"{now:%Y-%m-%d}")]
    if len(today) >= rules["per_day"]:
        return "We've made lots of stories today. Let's make a new one tomorrow! I can read you one of ours instead."
    if rules["not_after"]:
        h, m = (int(x) for x in rules["not_after"].split(":"))
        if (now.hour, now.minute) >= (h, m):
            return "It's getting late for a brand new story. I can read you one from the bookshelf instead."
    return None


def _result(story: dict, source: str = "") -> dict:
    """What app.py needs for one turn: the bubble line, the words to speak, and the public state."""
    scene = story["scenes"][-1]
    how = books.pace()
    gap = books._quiet(how["pause"]) or " "
    if story["done"]:
        reply = f'The end! "{story["title"]}" is on the bookshelf.'
        speak = gap.join([scene["narration"], "The end.",
                          f"That was our story, {story['title']}. I put it on the bookshelf so we can read it again."])
    else:
        a, b = (i[0].lower() + i[1:].rstrip(" .!") for i in scene["ideas"])
        ask = f"We could {a}, or {b}." + (" Or tell me your own idea!" if scene["n"] == 1 else "")
        reply = prompt.tidy(scene["question"])
        speak = gap.join([scene["narration"], scene["question"], ask])
    return {"reply": reply, "speak": speak, "speed": how["speed"], "source": source,
            "story": {"id": story["id"], "title": story["title"], "scene": scene["n"], "of": story["scenes_planned"],
                      "mood": scene["mood"], "ideas": scene["ideas"], "done": story["done"]}}


def _advance(story: dict, said: str) -> dict:
    said = _clean(said, 200)
    if NOT_FOR_STORIES.search(said) or PERSONAL.search(said):
        said = ""                                       # steered away from, and not kept
    for f in friends():                                 # he named an old friend himself: they are in
        first = f["name"].split()[0]
        if len(first) >= 3 and re.search(rf"\b{re.escape(first)}\b", said, re.I) and len(story["cast"]) < CAST_MAX \
                and f["name"].lower() not in (c.lower() for c in story["cast"]):
            story["cast"].append(f["name"])
    n = len(story["scenes"]) + 1
    beat = beat_for(n, story["scenes_planned"])
    scene, source = _scene(story, beat, said)
    ending = beat == "ending"
    story["scenes"].append({"n": n, "beat": beat, "said": said, "narration": _clean(scene["narration"], 900),
                            "question": "" if ending else _clean(scene["question"], 160),
                            "ideas": [] if ending else [_clean(scene["idea_a"], 80), _clean(scene["idea_b"], 80)],
                            "mood": scene["mood"] if scene["mood"] in MOODS else "happy", "source": source})
    title, friend = _clean(scene["title"], 60), _known_name(_clean(scene["new_friend"], 40))
    if not story["title"] and 0 < len(title.split()) <= 8:
        story["title"] = title.strip('"')
    if friend and len(friend.split()) <= 6 and len(story["cast"]) < CAST_MAX and \
            friend.lower() not in (c.lower() for c in story["cast"]):
        story["cast"].append(friend)
    story["place"] = _clean(scene["place"], 60) or story["place"]
    story["summary"] = _clean(scene["summary"], 400) or (story["summary"] + " " + story["scenes"][-1]["narration"])[-400:]
    if ending:
        _finish(story)
    else:
        _save(story)
        _set_active(story["id"])
    return _result(story, source)


def _finish(story: dict) -> None:
    """Mark it done and put it on the bookshelf under a title nobody else has."""
    story["done"] = True
    base = story["title"] or ("The Story of " + (story["idea"] or "Our Adventure")).title()[:60]
    taken = {b["name"] for b in books.library()}           # file names: "Winnie the Pooh" must not
    title, n = base, 2                                   # land on the file of "Winnie-the-Pooh"
    while books.slug(title) in taken:
        title, n = f"{base[:36].rstrip()} {n}", n + 1    # short enough that the number is in the file name
    story["title"] = title
    text = "\n\n".join(s["narration"] for s in story["scenes"])
    if books.add(title, f"{story['by'].title()} and Deadlee", text).startswith("ok"):
        story["book"] = books.slug(title)
    _save(story)
    _remember_friends(story)
    _set_active(None)


def start(idea: str, by: str = "", now: datetime | None = None) -> dict:
    """Begin a story. With no idea, Deadlee asks for one and the next thing said is the idea."""
    now = now or datetime.now(prompt.TZ)
    no = _may_start(now)
    if no:
        return {"reply": prompt.tidy(no), "speak": no, "speed": books.pace()["speed"], "story": None, "source": "limit"}
    idea = _clean(idea, 120)
    if NOT_FOR_STORIES.search(idea) or PERSONAL.search(idea):
        idea = ""
    if not idea:
        _set_active(None, waiting_for_idea=True, by=by)
        ask = "Ooh, let's make a story! What should it be about?"
        return {"reply": ask, "speak": ask, "speed": books.pace()["speed"], "story": None, "source": "ask"}
    return _advance(new_story(idea, by, now), "")


def turn(said: str) -> dict | None:
    """The child's answer during a story (or their idea, if Deadlee just asked for one)."""
    if waiting_for_idea():
        by = _mark().get("by", "")
        _set_active(None)
        return start(said, by)
    story = active()
    return _advance(story, said) if story else None


def stop() -> dict | None:
    """'Stop the story': close it kindly with the spare ending and keep what was made."""
    if waiting_for_idea():
        _set_active(None)
        line = "Okay, no story for now."
        return {"reply": line, "speak": line, "speed": books.pace()["speed"], "story": None, "source": "stop"}
    story = active()
    if not story:
        return None
    end = _fallback(story, "ending")
    story["scenes"].append({"n": len(story["scenes"]) + 1, "beat": "ending", "said": "", "narration": end["narration"],
                            "question": "", "ideas": [], "mood": end["mood"], "source": "stopped"})
    _finish(story)
    return _result(story, "stopped")


def resume() -> dict | None:
    """'Let's finish our story': the newest unfinished one from today, asked again where it stopped."""
    today = f"{datetime.now(prompt.TZ):%Y-%m-%d}"
    for entry in library():
        if not entry["done"] and entry["id"].startswith(today) and entry["scenes"]:
            story = _load(entry["id"])
            _set_active(story["id"])
            return _result(story, "resumed")
    return None
