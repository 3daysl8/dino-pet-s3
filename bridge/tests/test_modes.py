from datetime import datetime

import pytest

import books
import maths
import modes
import stories
from prompt import TZ

PARA = "Pooh walked through the forest humming a little hum to himself as he went. " * 12


def scene() -> dict:
    return {"narration": "We tiptoe past the mushroom and peek underneath it to see who is hiding there today. " * 3,
            "question": "What is hiding under the mushroom?", "idea_a": "Lift the mushroom", "idea_b": "Follow the butterfly",
            "mood": "curious", "title": "The Fairy Garden", "new_friend": "", "place": "a garden", "summary": "We found a garden."}


@pytest.fixture
def shelf(brain_dir, monkeypatch):
    books.add("Winnie-the-Pooh", "A. A. Milne", "Chapter 1\n\n" + PARA + "\n\n" + PARA)
    books.add("The Ugly Duckling", "Hans Christian Andersen", PARA)
    books.add("The Emperor's New Clothes", "Hans Christian Andersen", PARA)
    books.add("The Tale of Peter Rabbit", "Beatrix Potter", PARA)
    monkeypatch.setattr(stories, "ask_model", lambda system, user: scene())
    monkeypatch.setattr(stories, "_may_start", lambda now: None)
    monkeypatch.setattr(modes, "_shelf_at", 0.0)
    monkeypatch.setattr(modes, "_choice_at", 0.0)
    maths._rng.seed(3)
    return brain_dir


def test_what_a_parent_hit_is_no_longer_a_trap(shelf):
    """The real conversation from 2026-10-10, sentence for sentence."""
    first = modes.handle("Fairy games. Can you tell me a fairy tale?", "sam")
    assert first["speak"] == modes.CHOICE and not stories.active() and not stories.waiting_for_idea()
    second = modes.handle("How about you tell me a story from Hans Christian Andersen?", "sam")
    assert second["speak"] == ("I have 2 by Hans Christian Andersen: The Emperor's New Clothes or The Ugly Duckling. "
                               "Which one would you like?")
    third = modes.handle("I want you to read me a story from the library. What stories do we have?", "sam")
    assert third["speak"].startswith("I can read you The Emperor's New Clothes, The Tale of Peter Rabbit, ")
    assert third["speak"].endswith("or Winnie-the-Pooh. Which one would you like?")
    fourth = modes.handle("I want you to read me Winnie the Pooh", "sam")
    assert fourth["page"]["title"] == "Winnie-the-Pooh" and fourth["page"]["page"] == 1
    assert stories.library() == []                                   # no story was ever started


def test_asking_for_a_book_gets_out_of_a_story(shelf):
    assert modes.handle("Let's make up a story about a fairy garden", "sam")["story"]["scene"] == 1
    assert modes.handle("we read a magic book under the mushroom", "sam")["story"]["scene"] == 2   # plot
    out = modes.handle("I want you to read me Winnie the Pooh", "sam")
    assert out["page"]["title"] == "Winnie-the-Pooh" and stories.active() is None
    assert modes.handle("keep going", "sam", reading=True)["page"]["page"] == 2
    story = stories.library()[0]
    assert not story["done"] and story["scenes"] == 2                # kept, not finished, nothing added
    assert stories.resume()["story"]["scene"] == 2                   # "let's finish our story"
    assert modes.handle("stop", "sam")["story"]["done"]  # a bare "stop" ends it now


def test_out_of_the_idea_question_and_out_of_maths(shelf):
    assert modes.handle("let's make up a story", "sam")["reply"].startswith("Ooh, let's make a story!")
    assert stories.waiting_for_idea()
    out = modes.handle("Can you read me the ugly duckling", "sam")          # not taken as the idea
    assert out["page"]["title"] == "The Ugly Duckling" and not stories.waiting_for_idea() and stories.library() == []
    modes.handle("let's make up a story", "sam")
    assert modes.handle("never mind", "sam")["reply"] == "Okay, no story for now."

    assert modes.handle("let's do maths", "sam")["maths"]["n"] == 1
    assert modes.handle(str(maths.active()["question"]["answer"]), "sam")["maths"]["right"] == 1
    out = modes.handle("read me Peter Rabbit", "sam")
    assert out["page"]["title"] == "The Tale of Peter Rabbit" and maths.active() is None
    modes.handle("let's do maths", "sam")
    assert modes.handle("let's make a story about a robot", "sam")["story"]["scene"] == 1
    assert maths.active() is None
    assert modes.handle("can we do maths now", "sam")["maths"]["n"] == 1    # and out of the story
    assert stories.active() is None


def test_read_one_or_make_one_up(shelf):
    assert modes.handle("tell me a story", "sam")["speak"] == modes.CHOICE
    assert modes.handle("make one up", "sam")["reply"].startswith("Ooh, let's make a story!")
    stories.stop()
    modes.handle("can I have a story please", "sam")
    assert modes.handle("read one from the bookshelf", "sam")["speak"].startswith("I can read you ")
    assert modes.handle("the ugly duckling", "sam")["page"]["title"] == "The Ugly Duckling"   # just the title
    assert modes.handle("the ugly duckling", "sam") is None       # no longer being asked: the chat model's
    modes.handle("tell me a story", "sam")
    assert modes.handle("peter rabbit please", "sam")["page"]["title"] == "The Tale of Peter Rabbit"
    modes.handle("tell me a story", "sam")
    assert modes.handle("what's the weather like", "sam") is None and modes.handle("make one up", "sam") is None


def test_what_is_and_is_not_a_request_to_read(shelf):
    for said, title in (("read me Winnie the Pooh", "Winnie-the-Pooh"), ("Can you read the ugly duckling?", "The Ugly Duckling"),
                        ("please read us the emperors new suit", "The Emperor's New Clothes"),
                        ("read peter rabbit", "The Tale of Peter Rabbit")):
        assert books.wants_read(said)["book"]["title"] == title, said
    assert books.wants_read("read me a story")["titles"] == [b["title"] for b in books.library()]
    assert books.wants_read("what books do you have?")["direct"] and books.wants_read("read me a story")["direct"]
    assert books.wants_read("read me something by Anderson")["by"] == "Hans Christian Andersen"
    assert books.wants_read("a fairy tale by Hans Christian Andersen")["titles"] == ["The Emperor's New Clothes", "The Ugly Duckling"]
    assert books.wants_read("we read a magic book")["direct"] is False
    for said in ("keep reading", "go back a page", "read chapter 3 of Winnie the Pooh", "read that page again",
                 "what's the weather", "I like Hans Christian Andersen", "tell me a joke", "start Winnie the Pooh from the beginning"):
        assert books.wants_read(said) is None, said
    assert books.shelf_line({"titles": [], "by": ""}).startswith("There are no books")
    assert books.shelf_line({"titles": [f"Book {n}" for n in range(11)], "by": ""}) == \
        "I can read you Book 0, Book 1, Book 2, Book 3, Book 4, Book 5, Book 6, Book 7, and 3 more. Which one would you like?"


def test_what_is_making_a_story_and_what_is_vague():
    assert stories.wants_story("Let's make a story about a planet made of jellybeans!") == "a planet made of jellybeans"
    assert stories.wants_story("make up an adventure with a robot") == "a robot"
    assert stories.wants_story("tell me a story about dragons") == "dragons" and stories.wants_story("let's invent a story") == ""
    for said in ("Can you tell me a story", "Fairy games. Can you tell me a fairy tale?", "I want a story", "let's do a story"):
        assert stories.wants_story(said) is None and stories.vague(said), said
    for said in ("read me a story", "Can you read a story about bears", "I want you to read me a story", "what's the weather",
                 "I like stories"):
        assert stories.wants_story(said) is None and not stories.vague(said), said
    assert stories.wants_stop("stop the story") and stories.wants_stop("The end!") and stories.wants_stop("finish the story now")
    for said in ("stop", "Stop it.", "never mind", "no thank you", "I don't want a story"):
        assert stories.wants_stop(said), said
    assert not stories.wants_stop("we stop at the river") and not stories.wants_stop("and then the end of the rainbow")


def test_a_book_saved_in_another_encoding_does_not_stop_the_talking(shelf):
    with open(books._file("cafe-tales"), "wb") as f:
        f.write(b"Title: Caf\xe9 Tales\n\n" + PARA.encode())
    assert modes.handle("hello there", "sam") is None             # routing still runs on every sentence
    assert len(books.library()) == 5


def test_stopping_a_book_after_a_page(shelf):
    """The real conversation from 2026-10-14: a page, then "stop" went to the chat model,
    which stopped Sam's timer and said it wasn't reading a book."""
    page = modes.handle("Can you read me the story of Winnie the Pooh?", "sam")
    assert page["page"]["title"] == "Winnie-the-Pooh" and page["page"]["page"] == 1
    for said in ("Stop now", "No, why don't you just stop the book?", "stop reading please", "no",
                 "no thank you", "that's enough", "not now"):
        stop = modes.handle(said, "sam", reading=True)
        assert stop and "page" not in stop, said
        assert "Winnie-the-Pooh" in stop["speak"], said
    again = modes.handle("read me winnie the pooh", "sam")
    assert again["page"]["page"] == 2                                # stopping keeps the place


def test_stop_is_only_the_books_right_after_a_page(shelf):
    modes.handle("Can you read me the story of Winnie the Pooh?", "sam")
    assert modes.handle("Stop now", "sam") is None                # not reading: the model decides
    assert modes.handle("stop the timer", "sam", reading=True) is None
    assert modes.handle("turn the radio off, stop it", "sam", reading=True) is None
    assert modes.handle("keep going", "sam", reading=True)["page"]["page"] == 2
