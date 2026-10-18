import json
from datetime import datetime

import pytest

import books
import brain
import stories
import tools
from prompt import TZ

NOON = datetime(2026, 10, 11, 12, 0, tzinfo=TZ)
WORDS = "We float down the fizzy river on a liquorice raft and the sweet wind blows. " * 3      # 45 words


def scene(**change) -> dict:
    out = {"narration": WORDS.strip(), "question": "What do we do next?", "idea_a": "Taste the mountain",
           "idea_b": "Follow the fizzy river", "mood": "curious", "title": "The Jellybean Planet",
           "new_friend": "", "place": "the fizzy river", "summary": "We landed on a jellybean planet."}
    out.update(change)
    return out


@pytest.fixture
def model(brain_dir, monkeypatch):
    """A fake model: answers are popped from .queue (a dict, or an exception to raise); the
    default answer is a good scene. Every brief it was sent is kept in .asked."""
    class Fake:
        queue: list = []
        asked: list = []

        def __call__(self, system, user):
            self.asked.append((system, user))
            answer = self.queue.pop(0) if self.queue else scene()
            if isinstance(answer, Exception):
                raise answer
            return answer
    fake = Fake()
    fake.queue, fake.asked = [], []
    monkeypatch.setattr(stories, "ask_model", fake)
    return fake


def test_a_whole_story_from_his_idea_ends_up_on_the_bookshelf(model, brain_dir):
    first = stories.start("a planet made of jellybeans", "sam", NOON)
    assert first["story"] == {"id": "2026-10-11-a-planet-made-of-jellybeans", "title": "The Jellybean Planet",
                              "scene": 1, "of": 5, "mood": "curious", "done": False,
                              "ideas": ["Taste the mountain", "Follow the fizzy river"]}
    assert first["reply"] == "What do we do next?" and first["speed"] == 0.9
    assert first["speak"].endswith("We could taste the mountain, or follow the fizzy river. Or tell me your own idea!")
    assert "[pause:0.8s]" in first["speak"]
    assert stories.active()["id"] == first["story"]["id"] and not stories.waiting_for_idea()

    model.queue = [scene(new_friend="Zog the robot snail", place="a beeping cave")]
    second = stories.turn("a robot snail comes out!")
    assert second["story"]["scene"] == 2 and "Or tell me" not in second["speak"]
    assert stories.active()["cast"] == ["Sam", "Deadlee", "Zog the robot snail"]
    system, user = model.asked[-1]
    assert 'Sam said: "a robot snail comes out!"' in user and "Write scene 2 of 5." in user
    assert "Deadlee just asked: What do we do next? (ideas: Taste the mountain / Follow the fizzy river)" in user
    assert "The story so far: We landed on a jellybean planet." in user and "No roaring" in system

    stories.turn("follow the river")
    fourth = stories.turn("ask the gummy bears")
    assert "reach the big moment" in model.asked[-1][1] and fourth["story"]["scene"] == 4
    model.queue = [scene(question="", idea_a="", idea_b="", mood="proud")]
    last = stories.turn("build a drum")
    assert "This is the last scene" in model.asked[-1][1]
    assert last["story"]["done"] and last["reply"] == 'The end! "The Jellybean Planet" is on the bookshelf.'
    assert "The end." in last["speak"] and stories.active() is None and stories.turn("more") is None

    saved = json.load(open(brain_dir / "stories" / "2026-10-11-a-planet-made-of-jellybeans.json"))
    assert set(saved) == {"id", "title", "idea", "by", "started", "scenes_planned", "cast", "place", "summary",
                          "scenes", "done", "book"}
    assert [s["beat"] for s in saved["scenes"]] == ["opening", "middle", "middle", "turning", "ending"]
    assert [s["said"] for s in saved["scenes"]][1:3] == ["a robot snail comes out!", "follow the river"]
    assert saved["book"] == "the-jellybean-planet"
    book = books.read("jellybean planet")                    # and Deadlee can read it back
    assert book["title"] == "The Jellybean Planet" and book["author"] == "Sam and Deadlee"
    shelf = stories.library()[0]
    assert shelf["done"] and shelf["scenes"] == 5 and shelf["text"].count("liquorice raft") == 15


def test_bad_scenes_are_asked_for_again_then_replaced(model):
    model.queue = [{"narration": "too short"}, scene(narration=WORDS + "Then a zombie bites everyone.")]
    first = stories.start("a castle", "sam", NOON)                  # wrong shape, then unsuitable
    assert first["source"] == "fallback" and "fork in the road" in first["speak"]
    assert first["story"]["ideas"] == ["Climb the tall hill", "Follow the quiet stream"]
    assert [e["kind"] for e in brain.read_jsonl("events.jsonl")] == ["story_refused", "story_refused"]

    model.queue = [TimeoutError("slow"), scene(idea_b="Taste the mountain.")]     # down, then two same ideas
    assert stories.turn("climb the hill")["source"] == "fallback"
    model.queue = [ValueError("not json"), scene(title="")]
    assert stories.turn("ring the bell")["source"] == "retry"                      # the second try was fine
    model.queue = [scene(question="Where do you live?", title=""), scene(title="")]
    assert stories.turn("go on")["reply"] != "Where do you live?"
    model.queue = [RuntimeError("down"), RuntimeError("down")]
    end = stories.turn("hooray")                                                   # the ending has a spare too
    assert end["story"]["done"] and "shared a big picnic" in end["speak"]
    assert stories.library()[0]["title"] == "The Story Of A Castle"                # no title was ever accepted


def test_check_explains_what_is_wrong():
    assert stories.check(scene(), "middle") is None
    assert stories.check(scene(question="", idea_a="", idea_b=""), "ending") is None
    assert stories.check(scene(question="", idea_a="", idea_b=""), "middle") == "needs a question and two different ideas"
    assert stories.check(scene(narration="Hi."), "middle") == "narration is 1 words"
    assert stories.check(scene(idea_a="Shoot the dragon"), "middle") == "not suitable for a young child (shoot)"
    assert stories.check(scene(question="What is your school called? Tell me your school"), "middle") \
        == "asks for something personal"
    assert stories.check(scene(mood=3), "middle") == "wrong shape" and stories.check("text", "middle") == "wrong shape"
    assert stories.check(scene(narration=WORDS + "Smoke puffs up and a wooden sword glints."), "middle") is None
    for fine in ("Hello! The warm stable has a wardrobe.", "We diet on gunk and skill.", "A seashell and a diehard fan of beetles."):
        assert stories.check(scene(narration=WORDS + fine), "middle") is None, fine
    for bad, word in (("They go to war.", "war"), ("He was killed.", "killed"), ("A poisonous cake.", "poisonous")):
        assert stories.check(scene(narration=WORDS + bad), "middle") == f"not suitable for a young child ({word})"
    assert [stories.beat_for(n, 4) for n in range(1, 5)] == ["opening", "middle", "turning", "ending"]
    assert stories.beat_for(7, 8) == "turning" and stories.beat_for(9, 8) == "ending"


def test_his_unsuitable_idea_is_steered_not_kept(model):
    stories.start("pirates", "sam", NOON)
    stories.turn("we kill the pirate with a gun")
    assert "suggested something that doesn't suit" in model.asked[-1][1] and "gun" not in model.asked[-1][1]
    assert stories.active()["scenes"][-1]["said"] == ""
    model.queue = [scene(narration="Rawr! " + WORDS + "ROAR, said nobody.")]
    stories.turn("we find treasure")
    assert "rawr" not in stories.active()["scenes"][-1]["narration"].lower()
    assert "roar" not in stories.active()["scenes"][-1]["narration"].lower()


def test_asking_without_an_idea_then_stopping_and_resuming(model, monkeypatch):
    monkeypatch.setattr(stories, "_may_start", lambda now: None)
    asked = stories.start("", "sam")
    assert asked["reply"] == "Ooh, let's make a story! What should it be about?" and asked["story"] is None
    assert stories.waiting_for_idea() and stories.active() is None and not model.asked
    first = stories.turn("a dragon who loves pancakes")
    assert first["story"]["scene"] == 1 and stories.active()["idea"] == "a dragon who loves pancakes"

    stories.turn("flip a giant pancake")
    mark = json.load(open(stories._active_file()))                        # nobody says anything for a while
    mark["at"] -= stories.ACTIVE_S + 1
    open(stories._active_file(), "w").write(json.dumps(mark))
    assert stories.active() is None and stories.turn("hello?") is None and stories.stop() is None
    back = stories.resume()                                               # "let's finish our story"
    assert back["source"] == "resumed" and back["story"]["scene"] == 2 and stories.active()

    done = stories.stop()                                                 # "stop the story": kept, with an ending
    assert done["story"]["done"] and done["story"]["scene"] == 3 and "picnic" in done["speak"]
    assert len(model.asked) == 2 and books.find(done["story"]["title"])
    assert stories.resume() is None

    stories.start("", "sam")
    assert stories.stop()["reply"] == "Okay, no story for now." and not stories.waiting_for_idea()


def test_limits_and_settings(model, brain_dir):
    assert stories.settings() == {"scenes": 5, "per_day": 3, "not_after": "19:30"}
    late = stories.start("stars", "sam", NOON.replace(hour=19, minute=30))
    assert late["story"] is None and "getting late" in late["speak"] and not model.asked
    (brain_dir / "skills" / "stories.md").write_text("---\nscenes: 99\nper_day: 1\nnot_after:\n---\n- x\n")
    assert stories.settings() == {"scenes": 8, "per_day": 1, "not_after": ""}
    night = NOON.replace(hour=22)
    assert stories.start("stars", "sam", night)["story"]["of"] == 8        # no cut-off now
    assert "tomorrow" in stories.start("moons", "sam", night)["speak"]     # one a day
    assert stories.start("moons", "sam", night.replace(day=12))["story"]["scene"] == 1


def test_parent_can_delete_a_story_and_its_book(model):
    stories.start("a tiny whale", "sam", NOON)
    for said in ("a", "b", "c", "d"):
        stories.turn(said)
    entry = stories.library()[0]
    assert entry["done"] and books.find("jellybean planet")
    assert stories.remove("../../pet.token").startswith("error") and stories.remove("nope").startswith("error")
    assert stories.remove(entry["id"]) == "ok" and stories.library() == [] and books.library() == []


def test_the_tool_hands_the_scene_to_the_app(model, monkeypatch):
    monkeypatch.setattr(stories, "_may_start", lambda now: None)
    ctx = tools.Context(speaker="sam")
    out = tools.run("story", '{"action": "start", "idea": "a moon made of cheese"}', ctx)
    assert out.startswith("ok: the story is being told aloud") and "liquorice" not in out
    assert ctx.turn["story"]["scene"] == 1 and stories.active()["by"] == "sam"
    ctx = tools.Context()
    assert tools.run("story", '{"action": "stop"}', ctx).startswith("ok") and ctx.turn["story"]["done"]
    ctx = tools.Context()
    assert tools.run("story", '{"action": "continue"}', ctx).startswith("There is no story") and ctx.turn is None
    assert "story" in {t["function"]["name"] for t in tools.TOOLS}


def whole_story(model, idea, friend="", title="", when=NOON, said=("a", "b", "c", "d")):
    """Tell a five-scene story; the friend (if any) joins in scene 2."""
    model.queue = [scene(title=title), scene(new_friend=friend, title=title)]
    stories.start(idea, "sam", when)
    for words in said:
        stories.turn(words)
    return stories.library()[0]


def test_friends_from_finished_stories_can_come_back(model):
    assert stories.friends() == []
    stories.start("a pond", "sam", NOON)
    model.queue = [scene(new_friend="Zip the robot snail")]
    stories.turn("a snail")
    assert stories.friends() == []                                   # not until the story is finished
    for words in ("a", "b", "c"):
        stories.turn(words)
    first = stories.library()[0]
    assert [(f["name"], f["from"], f["stories"]) for f in stories.friends()] == \
        [("Zip the robot snail", "The Jellybean Planet", [first["id"]])]

    stories.start("the moon", "sam", NOON)                        # a new story is told about old friends
    assert "Friends from our other stories" in model.asked[-1][1]
    assert "Zip the robot snail (from The Jellybean Planet)" in model.asked[-1][1]
    model.queue = [scene(new_friend="zip")]                          # the model brings him back by first name
    stories.turn("we fly higher")
    assert stories.active()["cast"] == ["Sam", "Deadlee", "Zip the robot snail"]
    assert "Friends from our other stories" not in model.asked[-1][1] or "Zip" in model.asked[-1][1]
    stories.turn("x")
    assert "Friends from our other stories" not in model.asked[-1][1]      # he is already here: nobody left to offer
    for words in ("y", "z"):
        stories.turn(words)
    zip_ = stories.friends()[0]
    assert len(stories.friends()) == 1 and len(zip_["stories"]) == 2       # one friend, seen in two stories


def test_he_can_bring_a_friend_back_by_name_and_parents_can_remove_them(model):
    whole_story(model, "a pond", friend="Zip the robot snail", title="Pond Day")
    whole_story(model, "a hill", friend="Pip the gummy worm", title="Hill Day")
    assert [f["name"] for f in stories.friends()] == ["Pip the gummy worm", "Zip the robot snail"]

    model.queue = []
    stories.start("space", "sam", NOON)
    stories.turn("and then Zip comes to help us!")                   # said by the child, not proposed by the model
    assert "Zip the robot snail" in stories.active()["cast"] and "Pip the gummy worm" not in stories.active()["cast"]
    assert "Pip the gummy worm (from Hill Day)" in model.asked[-1][1]
    stories.stop()

    pond = next(s for s in stories.library() if s["title"] == "Pond Day")
    assert stories.remove(pond["id"]) == "ok"                        # Zip was also in the space story: he stays
    assert [f["name"] for f in stories.friends()] == ["Zip the robot snail", "Pip the gummy worm"]
    hill = next(s for s in stories.library() if s["title"] == "Hill Day")
    assert stories.remove(hill["id"]) == "ok"                        # Pip was only ever in that one
    assert [f["name"] for f in stories.friends()] == ["Zip the robot snail"]
    assert stories.remove_friend("zip the robot snail") == "ok" and stories.friends() == []
    assert stories.remove_friend("nobody").startswith("error")


def test_only_so_many_friends_are_kept_and_offered(model, monkeypatch):
    monkeypatch.setattr(stories, "FRIENDS_MAX", 3)
    monkeypatch.setattr(stories, "FRIENDS_SHOWN", 2)
    monkeypatch.setattr(stories, "_may_start", lambda now: None)
    clock = [1000.0]
    monkeypatch.setattr(stories.time, "time", lambda: clock.__setitem__(0, clock[0] + 1) or clock[0])
    for n, name in enumerate(["Ada the ant", "Bob the bee", "Cat the cat", "Dot the dog"]):
        whole_story(model, f"place {n}", friend=name, title=f"Story {n}")
    assert [f["name"] for f in stories.friends()] == ["Dot the dog", "Cat the cat", "Bob the bee"]
    model.queue = []
    stories.start("somewhere new", "sam", NOON)
    offered = model.asked[-1][1]
    assert "Dot the dog" in offered and "Cat the cat" in offered and "Bob the bee" not in offered


def test_the_second_try_is_told_which_word_to_leave_out(model):
    model.queue = [scene(narration=WORDS + "The pirate waves a gun."), scene()]
    assert stories.start("a boat", "sam", NOON)["source"] == "retry"
    assert "Do not use the word" not in model.asked[0][1]
    assert 'Do not use the word "gun"' in model.asked[1][1]
    assert brain.read_jsonl("events.jsonl")[-1]["detail"] == "not suitable for a young child (gun)"


def test_a_story_never_takes_a_book_s_file(model):
    """'Winnie the Pooh' and 'Winnie-the-Pooh' are different titles that make the same file name."""
    books.add("Winnie-the-Pooh", "A. A. Milne", WORDS * 3)
    model.queue.append(scene(title="Winnie the Pooh"))
    stories.start("winnie the pooh", "sam", NOON)
    stories.stop()
    assert {b["name"]: b["author"] for b in books.library()} == {
        "winnie-the-pooh": "A. A. Milne", "winnie-the-pooh-2": "Sam and Deadlee"}
    assert stories.library()[0]["book"] == "winnie-the-pooh-2"


def test_a_slow_model_gets_no_second_try(model, monkeypatch):
    """The toy only waits 30 s, so a slow first try goes straight to the hand-written scene."""
    monkeypatch.setattr(stories, "RETRY_WITHIN_S", -1)
    model.queue.append(RuntimeError("slow"))
    out = stories.start("a jellybean planet", "sam", NOON)
    assert out["source"] == "fallback" and len(model.asked) == 1
