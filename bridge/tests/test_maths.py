import json
import re

import pytest

import brain
import maths
import tools


@pytest.fixture
def game(brain_dir):
    maths._rng.seed(7)
    return brain_dir


def right():
    return str(maths.active()["question"]["answer"])


def wrong():
    return str(maths.active()["question"]["answer"] + 1)


def test_questions_fit_their_level_and_the_answers_are_right():
    maths._rng.seed(1)
    for level, top in ((0, 20), (1, 10), (2, 20), (3, 60), (4, 100), (5, 100)):
        for _ in range(300):
            q = maths.question(level)
            assert set(q) == {"show", "ask", "answer"} and 0 <= q["answer"] <= top, (level, q)
            nums = [int(n) for n in re.findall(r"\d+", q["ask"])]
            if " plus " in q["ask"]:
                assert q["answer"] == sum(nums)
            elif " take away " in q["ask"]:
                assert q["answer"] == nums[0] - nums[1] and q["answer"] >= 1
            elif " times " in q["ask"]:
                assert q["answer"] == nums[0] * nums[1] and (level == 5 or nums[1] in (2, 5, 10))
            elif "double" in q["ask"]:
                assert q["answer"] == 2 * nums[0]
            elif "half" in q["ask"]:
                assert q["answer"] * 2 == nums[0]
            elif "after" in q["ask"]:
                assert q["answer"] == nums[0] + 1
            else:
                assert "bigger" in q["ask"] and q["answer"] == max(nums) and nums[0] != nums[1]
    assert maths.tell({"ask": "What is 4 plus 3?", "answer": 7}) == "4 plus 3 is 7."
    assert maths.tell({"ask": "What is double 6?", "answer": 12}) == "Double 6 is 12."
    assert maths.tell({"ask": "What number comes after 7?", "answer": 8}) == "After 7 comes 8."
    assert maths.tell({"ask": "Which is bigger, 5 or 8?", "answer": 8}) == "8 is bigger."


def test_numbers_are_heard_however_they_are_said():
    for said, n in (("7", 7), ("Seven!", 7), ("um, is it fifteen?", 15), ("twenty one", 21), ("Twenty-one.", 21),
                    ("forty", 40), ("it's 3 no 4", 4), ("a hundred", 100), ("ate", 8), ("Too.", 2), ("zero", 0)):
        assert maths.heard_number(said) == n, said
    for said in ("", "I don't know", "banana", "for the win"):
        assert maths.heard_number(said) is None, said


def test_phrases_that_start_a_game():
    for yes in ("Let's do maths", "can we play the maths game", "I want to practise my sums", "quiz me",
                "Let's do some times tables", "maths time!"):
        assert maths.wants_maths(yes), yes
    for no in ("what's 7 plus 8", "I did maths at school today", "what is half of 20", "tell me a joke"):
        assert not maths.wants_maths(no), no


def test_a_whole_game_right_first_time_moves_up_a_level(game):
    first = maths.start("sam")
    assert first["speak"].startswith("Let's do some maths, Sam! I'll ask 8 questions. What is ")
    assert first["maths"] == {"question": maths.active()["question"]["show"], "n": 1, "of": 8, "right": 0,
                              "level": 1, "done": False}
    assert first["reply"] == maths.active()["question"]["show"] and first["speed"] == 1.0
    turns = [maths.answer(right()) for _ in range(8)]
    assert "trickier" in turns[2]["speak"] and turns[2]["maths"]["level"] == 2       # three in a row
    assert turns[5]["maths"]["level"] == 3 and turns[6]["maths"]["n"] == 8
    end = turns[7]
    assert end["maths"]["done"] and "You got 8 out of 8. Fantastic!" in end["speak"] and end["reply"] == "8 out of 8. Fantastic!"
    assert maths.active() is None and maths.answer("7") is None
    saved = maths.progress()["sam"]
    assert saved["level"] == 3 and saved["level_name"] == "bigger sums, doubles and halves"
    assert [(g["asked"], g["right"], g["first_try"]) for g in saved["games"]] == [(8, 8, 8)]
    assert maths.start("sam")["maths"]["level"] == 3                              # remembered next time


def test_wrong_answers_get_a_second_go_then_the_answer_kindly(game):
    maths.start("sam")
    q = dict(maths.active()["question"])
    again = maths.answer(wrong())
    assert again["speak"].endswith(q["ask"]) and again["maths"]["n"] == 1 and again["maths"]["right"] == 0
    back = maths.answer(right())                                     # right on the second go still counts
    assert back["maths"] == {"question": maths.active()["question"]["show"], "n": 2, "of": 8, "right": 1,
                             "level": 1, "done": False}
    q = dict(maths.active()["question"])
    maths.answer(wrong())
    shown = maths.answer(wrong())                                    # second miss: told the answer, no telling off
    assert maths.tell(q) in shown["speak"] and shown["maths"]["n"] == 3 and shown["maths"]["right"] == 1
    assert not re.search(r"wrong|no,|incorrect|bad", shown["speak"], re.I)
    maths.answer(wrong()), maths.answer(wrong())                     # two shown in a row: an easier level
    assert maths.active()["level"] == 0 and maths.active()["n"] == 4

    q = dict(maths.active()["question"])
    hmm = maths.answer("banana")
    assert hmm["speak"] == "I didn't catch a number. " + q["ask"] and maths.active()["n"] == 4
    skipped = maths.answer("I don't know")
    assert "That's okay. " + maths.tell(q) in skipped["speak"] and maths.active()["n"] == 5
    done = maths.answer("stop please")
    assert done["maths"]["done"] and "You got 1 out of 4." in done["speak"] and maths.active() is None
    game_saved = maths.progress()["sam"]["games"][0]
    assert (game_saved["asked"], game_saved["right"], game_saved["first_try"]) == (4, 1, 0)


def test_stopping_before_answering_and_settings_and_reset(game):
    maths.start("sam")
    assert maths.stop()["speak"] == "Okay, we can do maths another time." and maths.progress()["sam"]["games"] == []
    assert maths.stop() is None
    assert maths.settings() == {"questions": 8, "start_level": 1, "speed": 1.0}
    (game / "skills" / "maths.md").write_text("---\nquestions: 3\nstart_level: 4\nspeed: 0.2\n---\n- x\n")
    assert maths.settings() == {"questions": 3, "start_level": 4, "speed": 0.6}
    assert maths.start("sam")["maths"]["level"] == 1                  # his saved level, not the starting one
    maths.stop()
    first = maths.start("mum")
    assert first["maths"]["level"] == 4 and first["maths"]["of"] == 3 and " times " in first["speak"]
    for _ in range(3):
        last = maths.answer(right())
    assert last["maths"]["done"] and maths.progress()["mum"]["level"] == 5
    assert maths.reset("mum") == "ok" and "mum" not in maths.progress() and maths.reset("mum").startswith("error")

    mark = json.load(open(maths._file("active.json")))                   # a game left alone is over
    maths.start("sam")
    mark = json.load(open(maths._file("active.json")))
    mark["at"] -= maths.ACTIVE_S + 1
    open(maths._file("active.json"), "w").write(json.dumps(mark))
    assert maths.active() is None and maths.answer("3") is None


def test_the_tool_hands_the_turn_to_the_app(game):
    ctx = tools.Context(speaker="sam")
    out = tools.run("maths_game", '{"action": "start"}', ctx)
    assert out.startswith("ok: the maths game is talking") and ctx.turn["maths"]["n"] == 1
    ctx = tools.Context()
    assert tools.run("maths_game", '{"action": "stop"}', ctx).startswith("ok") and ctx.turn["maths"] is None
    ctx = tools.Context()
    assert tools.run("maths_game", '{"action": "stop"}', ctx) == "No maths game is being played." and ctx.turn is None
    assert "maths_game" in {t["function"]["name"] for t in tools.TOOLS}
    assert "maths_game" in brain.snapshot().skills and "start_level:" not in brain.snapshot().skills
