"""Maths game: Deadlee asks sums at the child's level and checks the answers.

    brain/maths/progress.json   per child: the level they are on and a line per game (counts only)
    brain/maths/active.json     the game being played right now
    brain/skills/maths.md       settings at the top: questions, start_level, speed

No model is involved: the questions are made here and the answers are checked here, so a
sum is never wrong and a turn takes only as long as the voice. Two tries a question, a
kind word either way, and the level drifts up after three right first time in a row and
down after two that needed the answer given.
"""
import json
import os
import random
import re
import time
from datetime import datetime

import brain
import prompt

ACTIVE_S = 10 * 60
SETTINGS = {"questions": (8, 3, 20), "start_level": (1, 0, 5)}       # default, min, max
SPEED = (1.0, 0.6, 1.3)
LEVELS = ["counting and which is bigger", "adding and taking away to 10", "adding and taking away to 20",
          "bigger sums, doubles and halves", "times tables: 2, 5 and 10", "all the times tables"]
UP_AFTER, DOWN_AFTER = 3, 2
GAMES_KEPT = 30

RIGHT = ["Yes!", "That's right!", "You got it!", "Spot on!", "Correct!", "Nice work!", "Brilliant!"]
AGAIN = ["Not quite. Have another go.", "Nearly! Try once more.", "Good try. Have one more go."]
SHOWN = ["Good try!", "That was a tricky one.", "Nice thinking."]

WANTS_MATHS = re.compile(
    r"\b(?:do|play|practi[sc]e|start|have|try|want)\b.{0,25}\b(?:maths?|sums|numbers? game|times tables?)\b|"
    r"\bmaths? (?:game|quiz|time|questions?)\b|\b(?:quiz|test) me\b", re.I)
WANTS_STOP = re.compile(r"\b(?:stop|finish|end|no more|enough|i'?m done|all done)\b", re.I)
WANTS_SKIP = re.compile(r"\b(?:i don'?t know|don'?t know|not sure|skip|pass|too hard|tell me)\b", re.I)

UNITS = ["zero", "one", "two", "three", "four", "five", "six", "seven", "eight", "nine", "ten", "eleven", "twelve",
         "thirteen", "fourteen", "fifteen", "sixteen", "seventeen", "eighteen", "nineteen"]
TENS = {"twenty": 20, "thirty": 30, "forty": 40, "fifty": 50, "sixty": 60, "seventy": 70, "eighty": 80, "ninety": 90}
SOUNDS_LIKE = {"won": 1, "to": 2, "too": 2, "tree": 3, "free": 3, "for": 4, "fore": 4, "ate": 8, "nein": 9, "tin": 10}

_rng = random.Random()


# ---------------------------------------------------------------- files and settings

def _file(name: str) -> str:
    return brain.path("maths", name)


def _read(name: str) -> dict:
    try:
        return json.load(open(_file(name), encoding="utf-8"))
    except (OSError, ValueError):
        return {}


def settings() -> dict:
    wanted = brain.front_matter("maths")
    out = {k: v[0] for k, v in SETTINGS.items()}
    for k, (_, lo, hi) in SETTINGS.items():
        try:
            out[k] = min(max(int(float(wanted[k])), lo), hi)
        except (KeyError, ValueError):
            pass
    try:
        out["speed"] = min(max(float(wanted["speed"]), SPEED[1]), SPEED[2])
    except (KeyError, ValueError):
        out["speed"] = SPEED[0]
    return out


def progress() -> dict:
    """For the parent's page: {child: {level, level_name, games: [{date, asked, right, level}]}}."""
    out = {}
    for child, p in _read("progress.json").items():
        out[child] = {"level": p.get("level", 0), "level_name": LEVELS[p.get("level", 0)], "games": p.get("games", [])}
    return out


def reset(child: str) -> str:
    data = _read("progress.json")
    if data.pop(child.strip().lower(), None) is None:
        return "error: nobody by that name has played"
    brain._write(_file("progress.json"), json.dumps(data, indent=1))
    return "ok"


def active() -> dict | None:
    game = _read("active.json")
    return game if game.get("question") and time.time() - game.get("at", 0) < ACTIVE_S else None


def _keep(game: dict | None) -> None:
    if game:
        game["at"] = time.time()
    brain._write(_file("active.json"), json.dumps(game or {}))


# ---------------------------------------------------------------- questions

def question(level: int) -> dict:
    """One question for a level: {show, ask, answer}. show is for the bubble, ask is spoken."""
    r = _rng
    if level <= 0:
        if r.random() < 0.5:
            n = r.randint(1, 18)
            return {"show": f"after {n}?", "ask": f"What number comes after {n}?", "answer": n + 1}
        a, b = r.sample(range(1, 20), 2)
        return {"show": f"bigger: {a} or {b}?", "ask": f"Which is bigger, {a} or {b}?", "answer": max(a, b)}
    if level in (1, 2):
        top = 10 if level == 1 else 20
        if r.random() < 0.5:
            a = r.randint(1, top - 1)
            b = r.randint(1, top - a)
            return {"show": f"{a} + {b} = ?", "ask": f"What is {a} plus {b}?", "answer": a + b}
        a = r.randint(2, top)
        b = r.randint(1, a - 1)
        return {"show": f"{a} - {b} = ?", "ask": f"What is {a} take away {b}?", "answer": a - b}
    if level == 3:
        kind = r.choice(["double", "half", "plus", "minus"])
        if kind == "double":
            n = r.randint(3, 15)
            return {"show": f"double {n}?", "ask": f"What is double {n}?", "answer": 2 * n}
        if kind == "half":
            n = 2 * r.randint(2, 15)
            return {"show": f"half of {n}?", "ask": f"What is half of {n}?", "answer": n // 2}
        if kind == "plus":
            a, b = r.randint(11, 39), r.randint(2, 10)
            return {"show": f"{a} + {b} = ?", "ask": f"What is {a} plus {b}?", "answer": a + b}
        a, b = r.randint(15, 50), r.randint(2, 10)
        return {"show": f"{a} - {b} = ?", "ask": f"What is {a} take away {b}?", "answer": a - b}
    a = r.choice([2, 5, 10]) if level == 4 else r.randint(2, 10)
    b = r.randint(1, 10)
    return {"show": f"{b} x {a} = ?", "ask": f"What is {b} times {a}?", "answer": a * b}


def tell(q: dict) -> str:
    """The answer as a sentence: '4 plus 3 is 7.', 'After 7 comes 8.', '8 is bigger.'"""
    ask = q["ask"].rstrip("?")
    if ask.startswith("What number comes after "):
        return f"After {ask.rsplit(' ', 1)[1]} comes {q['answer']}."
    if ask.startswith("Which is bigger"):
        return f"{q['answer']} is bigger."
    sum_ = ask.replace("What is ", "", 1)
    return f"{sum_[0].upper()}{sum_[1:]} is {q['answer']}."


def heard_number(heard: str) -> int | None:
    """The last number in what was said: '7', 'seven', 'twenty one', 'um, is it fifteen?'."""
    words = re.findall(r"[a-z]+|\d+", heard.lower().replace("-", " "))
    found, i = [], 0
    while i < len(words):
        w = words[i]
        if w.isdigit():
            found.append(int(w))
        elif w in TENS:
            unit = UNITS.index(words[i + 1]) if i + 1 < len(words) and words[i + 1] in UNITS[1:10] else 0
            found.append(TENS[w] + unit)
            i += 1 if unit else 0
        elif w in UNITS:
            found.append(UNITS.index(w))
        elif w == "hundred":
            found.append(100)
        i += 1
    if not found and len(words) == 1 and words[0] in SOUNDS_LIKE:      # Whisper heard "ate" for "eight"
        return SOUNDS_LIKE[words[0]]
    return found[-1] if found else None


def wants_maths(heard: str) -> bool:
    return bool(WANTS_MATHS.search(heard))


# ---------------------------------------------------------------- playing

def _turn(game: dict | None, said: str, ask: str = "", show: str = "", done: bool = False) -> dict:
    speak = " ".join(x for x in (said, ask) if x)
    info = None
    if game:
        info = {"question": game["question"]["show"] if not done else "", "n": game["n"], "of": game["of"],
                "right": game["right"], "level": game["level"], "done": done}
    return {"reply": prompt.tidy(show or ask or said), "speak": speak, "speed": settings()["speed"], "maths": info}


def start(by: str = "") -> dict:
    child = (by or brain.MAIN_PERSON).lower()
    rules = settings()
    level = _read("progress.json").get(child, {}).get("level", rules["start_level"])
    game = {"by": child, "level": level, "n": 1, "of": rules["questions"], "right": 0, "first_try": 0, "tries": 0,
            "streak": 0, "misses": 0, "question": question(level)}
    _keep(game)
    return _turn(game, f"Let's do some maths, {child.title()}! I'll ask {game['of']} questions.",
                 game["question"]["ask"], game["question"]["show"])


def _finish(game: dict) -> dict:
    data = _read("progress.json")
    p = data.setdefault(game["by"], {"games": []})
    p["level"] = game["level"]
    asked = game["n"] - (0 if game.get("answered") else 1)
    if asked:
        p["games"] = (p["games"] + [{"date": f"{datetime.now(prompt.TZ):%Y-%m-%d}", "asked": asked,
                                     "right": game["right"], "first_try": game["first_try"],
                                     "level": game["level"]}])[-GAMES_KEPT:]
    brain._write(_file("progress.json"), json.dumps(data, indent=1))
    _keep(None)
    if not asked:
        return _turn(None, "Okay, we can do maths another time.")
    praise = "Fantastic!" if game["right"] == asked else "Great work!" if game["right"] * 2 >= asked else "Good trying!"
    return _turn(game, f"That's the end of our maths game. You got {game['right']} out of {asked}. {praise}",
                 show=f"{game['right']} out of {asked}. {praise}", done=True)


def _next(game: dict, said: str) -> dict:
    """Move to the next question (or finish), adjusting the level first."""
    game["answered"] = True
    if game["streak"] >= UP_AFTER and game["level"] < len(LEVELS) - 1:
        game["level"], game["streak"] = game["level"] + 1, 0
        said += " You're doing so well, let's try some trickier ones."
    elif game["misses"] >= DOWN_AFTER and game["level"] > 0:
        game["level"], game["misses"] = game["level"] - 1, 0
    if game["n"] >= game["of"]:
        done = _finish(game)
        done["speak"] = f"{said} {done['speak']}"
        return done
    game["n"], game["tries"], game["answered"] = game["n"] + 1, 0, False
    game["question"] = question(game["level"])
    _keep(game)
    return _turn(game, said, game["question"]["ask"], game["question"]["show"])


def answer(heard: str) -> dict | None:
    """What the child said during a game."""
    game = active()
    if not game:
        return None
    q = game["question"]
    told = tell(q)
    number = heard_number(heard)
    if number is None:
        if WANTS_STOP.search(heard):
            return _finish(game)
        if WANTS_SKIP.search(heard):
            game["streak"], game["misses"] = 0, game["misses"] + 1
            return _next(game, f"That's okay. {told}")
        _keep(game)
        return _turn(game, "I didn't catch a number.", q["ask"], q["show"])
    if number == q["answer"]:
        game["right"] += 1
        if game["tries"] == 0:
            game["first_try"], game["streak"], game["misses"] = game["first_try"] + 1, game["streak"] + 1, 0
        return _next(game, RIGHT[(game["n"] + game["right"]) % len(RIGHT)])
    if game["tries"] == 0:                              # one more go before the answer is given
        game["tries"], game["streak"] = 1, 0
        _keep(game)
        return _turn(game, AGAIN[game["n"] % len(AGAIN)], q["ask"], q["show"])
    game["misses"] += 1
    return _next(game, f"{SHOWN[game['n'] % len(SHOWN)]} {told}")


def stop() -> dict | None:
    game = active()
    return _finish(game) if game else None
