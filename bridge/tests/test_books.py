import books
import brain
import tools

PARA = "Peter ran as fast as he could go. " * 12            # 96 words


def shelf(brain_dir):
    assert books.add("The Tale of Peter Rabbit", "Beatrix Potter", "\n\n".join([PARA] * 7)).startswith("ok")
    assert books.add("Winnie-the-Pooh", "A. A. Milne", "CHAPTER I\n\n" + PARA + "\n\nCHAPTER II\n\n" + PARA) == "ok (2 pages)"


def test_pages_are_about_a_minute_and_end_on_a_paragraph():
    pages = books.paginate("\n\n".join([PARA] * 7))
    assert [len(p.split()) for p in pages] == [192, 192, 192, 96]
    assert all(p.endswith("go.") for p in pages)
    long = books.paginate("A short sentence here. " * 200)                  # one 800-word paragraph
    assert len(long) >= 4 and all(len(p.split()) <= 260 and p.endswith(".") for p in long)
    wrapped = books.paginate("Once upon\n   a time\nthere were four.\n\n\n\nNext.")
    assert wrapped == ["Once upon a time there were four.\n\nNext."]


def test_empty_shelf(brain_dir):
    assert books.library() == [] and books.read("Peter Rabbit") is None and books.read() is None
    assert "no books" in books.shelf_text()
    assert books.add("", "x", PARA).startswith("error") and books.add("T", "x", "too short").startswith("error")
    assert books.remove("../../pet.token").startswith("error") and books.remove("nope").startswith("error")


def test_reading_moves_the_bookmark_and_finishing_starts_again(brain_dir):
    shelf(brain_dir)
    assert [b["title"] for b in books.library()] == ["The Tale of Peter Rabbit", "Winnie-the-Pooh"]
    assert books.find("peter rabbit")["name"] == "the-tale-of-peter-rabbit"
    assert books.find("Winnie the Pooh")["title"] == "Winnie-the-Pooh" and books.find("pooh bear")
    assert books.find("the gruffalo") is None
    first = books.read("Peter Rabbit")
    assert (first["page"], first["pages"], first["first"], first["last"]) == (1, 4, True, False)
    assert books.spoken(first).startswith("The Tale of Peter Rabbit, by Beatrix Potter. [pause:0.8s] Peter ran")
    assert books.spoken(first).endswith("Shall I keep going?")
    assert books.bubble(first) == "The Tale of Peter Rabbit: page 1 of 4."
    assert books.read()["page"] == 2                         # no title: the book last read
    assert books.read("pooh")["page"] == 1
    assert books.read()["title"] == "Winnie-the-Pooh"        # now Pooh is the last one read
    assert books.read("peter rabbit")["page"] == 3
    last = books.read("peter rabbit")
    assert last["last"] and books.spoken(last).endswith("The end.") and books.bubble(last).endswith("The end!")
    assert not books.spoken(last).startswith("The Tale")
    assert books.read("peter rabbit")["page"] == 1           # finished: next time from the start
    assert books.read("peter rabbit", "start")["page"] == 1
    assert "up to page 2 of 4" in books.shelf_text() and "by A. A. Milne" in books.shelf_text()
    assert books.remove("winnie-the-pooh") == "ok" and len(books.library()) == 1


def test_keep_going_is_recognised_without_the_model():
    for yes in ("Keep going!", "keep reading please", "More", "yes please", "next page", "And then what happened?",
                "Okay.", "read some more"):
        assert books.wants_more(yes), yes
    for no in ("", "stop", "what does mischief mean", "keep going to the shops", "no more", "read me another book"):
        assert not books.wants_more(no), no


GUTENBERG = (
    "﻿The Project Gutenberg eBook of The Tale of Peter Rabbit\r\n\r\nTitle: The Tale of Peter Rabbit\r\n\r\n"
    "Author: Beatrix Potter\r\n\r\n*** START OF THE PROJECT GUTENBERG EBOOK THE TALE OF PETER RABBIT ***\r\n\r\n"
    "[Illustration]\r\n\r\nFREDERICK WARNE\r\n\r\nFirst published 1902\r\n\r\n[Illustration: Peter\r\nin a jacket]\r\n\r\n"
    "Once upon a time there were four little Rabbits, and their names\r\nwere--\r\n\r\n          Flopsy,\r\n"
    "       Mopsy,\r\n\r\n       *       *       *\r\n\r\n_Cotton-tail_, and Peter.\r\n\r\n\r\n\r\nTHE END\r\n\r\n"
    "*** END OF THE PROJECT GUTENBERG EBOOK THE TALE OF PETER RABBIT ***\r\n\r\nLicence text here.\r\n")


def test_gutenberg_files_lose_their_wrapping():
    title, author, body = books.clean_gutenberg(GUTENBERG, "once upon a time")
    assert (title, author) == ("The Tale of Peter Rabbit", "Beatrix Potter")
    assert body.startswith("Once upon a time there were four little Rabbits") and body.endswith("and Peter.")
    for junk in ("Illustration", "Gutenberg", "Licence", "FREDERICK", "_", "\r", "\n\n\n", "*", "THE END"):
        assert junk not in body, junk
    assert "Cotton-tail, and Peter." in body
    assert "FREDERICK WARNE" in books.clean_gutenberg(GUTENBERG)[2]          # no start words: keep it all


def test_gutenberg_repeated_header_and_capital_headings():
    raw = ("Title: Winnie-the-Pooh\n\nAuthor: A. A. Milne\n\n*** START OF THE PROJECT GUTENBERG EBOOK WINNIE-THE-POOH ***\n\n"
           "The Project Gutenberg eBook of Winnie-the-Pooh\n\nThis eBook is for the use of anyone.\n\n"
           "*** START OF THE PROJECT GUTENBERG EBOOK WINNIE-THE-POOH ***\n\n"
           "                 CHAPTER IV\n\n        IN WHICH EEYORE LOSES A TAIL\n          AND POOH FINDS ONE\n\n"
           "The Old Grey Donkey, Eeyore, stood by himself.\n\nBANG!!!???***!!!\n\n"
           "[Transcriber's Note: something\nwas changed]\n\n*** END OF THE PROJECT GUTENBERG EBOOK WINNIE-THE-POOH ***\n")
    title, author, body = books.clean_gutenberg(raw)
    assert body == ("Chapter 4\n\nIn which eeyore loses a tail and pooh finds one\n\n"
                    "The Old Grey Donkey, Eeyore, stood by himself.\n\nBANG!!!???***!!!")
    assert (title, author) == ("Winnie-the-Pooh", "A. A. Milne")
    assert books._heading("CHAPTER XIV.") == "Chapter 14" and books._heading("FREDERICK WARNE") == "FREDERICK WARNE"
    assert len(books.paginate(body + "\n\nChapter 5\n\nMore.")) == 2            # a chapter starts a page


def test_the_tool_hands_the_page_to_the_app_not_the_model(brain_dir):
    shelf(brain_dir)
    ctx = tools.Context()
    out = tools.run("read_book", '{"title": "Peter Rabbit"}', ctx)
    assert out.startswith("ok: page 1 of 4") and "Peter ran" not in out
    assert ctx.turn["page"]["page"] == 1 and ctx.turn["page"]["text"].startswith("Peter ran")
    ctx = tools.Context()
    assert tools.run("read_book", '{"from": "list"}', ctx).startswith("Books on the shelf:") and ctx.turn is None
    out = tools.run("read_book", '{"title": "The Gruffalo"}', ctx)
    assert out.startswith("That book isn't on the shelf. Books on the shelf:") and ctx.turn is None
    assert tools.run("read_book", '{"title": "Peter Rabbit", "from": "start"}', ctx).startswith("ok: page 1 of 4")
    assert "read_book" in {t["function"]["name"] for t in tools.TOOLS}


def test_moving_around_a_book(brain_dir):
    import pytest
    text = "\n\n".join(f"Chapter {i}\n\n{PARA}\n\n{PARA}\n\n{PARA}" for i in range(1, 4))
    assert books.add("Pooh", "Milne", text) == "ok (6 pages)"            # two pages a chapter
    assert books.chapters(books.paginate(text)) == {1: 0, 2: 2, 3: 4}
    assert books.chapters(["CHAPTER IV\n\nx", "words", "Chapter 5 y"]) == {4: 0, 5: 2}
    assert books.read("pooh", "again")["page"] == 1                      # nothing read yet: page 1
    assert [books.read("pooh")["page"] for _ in range(2)] == [2, 3]
    again = books.read("pooh", "again")
    assert again["page"] == 3 and again["jumped"] and books.spoken(again).startswith("Page 3. [pause:0.8s] Chapter 2")
    assert books.read("pooh", "back")["page"] == 2                       # the one before the page just read
    assert books.read("pooh", "back")["page"] == 1
    first = books.read("pooh", "back")                                   # can't go before the beginning
    assert first["page"] == 1 and books.spoken(first).startswith("Pooh, by Milne.")
    assert books.read("pooh", "forward")["page"] == 3                    # skips page 2
    assert books.read("pooh")["page"] == 4 and not books.read("pooh", "start")["jumped"]
    ch3 = books.read("pooh", chapter=3)
    assert ch3["page"] == 5 and books.spoken(ch3).startswith("Page 5. [pause:0.8s] Chapter 3")
    assert books.read("pooh")["page"] == 6                               # carries on from the jump
    assert books.read("pooh", "again")["page"] == 6                      # finished: "again" is the last page
    assert books.read("pooh", page=2)["page"] == 2 and books.read()["page"] == 3
    with pytest.raises(ValueError, match="pages 1 to 6"):
        books.read("pooh", page=7)
    with pytest.raises(ValueError, match="chapters 1 to 3"):
        books.read("pooh", chapter=9)
    books.add("Plain", "", PARA)
    with pytest.raises(ValueError, match="no chapters, only pages"):
        books.read("plain", chapter=2)
    assert books.read("pooh")["page"] == 4                               # a refused jump moves nothing

    assert books.set_bookmark("pooh", 6) == "ok" and books.read("pooh")["page"] == 6
    assert books.set_bookmark("pooh", 7).startswith("error") and books.set_bookmark("../x", 1).startswith("error")

    ctx = tools.Context()
    assert tools.run("read_book", '{"title": "pooh", "chapter": 2}', ctx).startswith("ok: page 3 of 6")
    assert ctx.turn["page"]["jumped"]
    ctx = tools.Context()
    assert tools.run("read_book", '{"title": "pooh", "page": 40}', ctx) == "error: Pooh has pages 1 to 6."
    assert ctx.turn is None
    assert tools.run("read_book", '{"from": "back"}', ctx).startswith("ok: page 2 of 6")


def test_reading_pace_comes_from_the_books_skill(brain_dir):
    assert books.pace() == {"speed": 0.9, "pause": 0.8, "sentence_pause": 0.0}      # the template's
    assert "speed:" not in brain.snapshot().skills and "read_book" in brain.snapshot().skills
    page = {"title": "T", "author": "A", "page": 2, "pages": 3, "first": False, "last": False,
            "text": "One. Two!\n\n'Three,' said\n   Peter. Four?"}
    assert books.spoken(page) == "One. Two! [pause:0.8s] 'Three,' said Peter. Four? [pause:0.8s] Shall I keep going?"

    (brain_dir / "skills" / "books.md").write_text(
        "---\nspeed: 0.75  # slow\npause: 1.5\nsentence_pause: 0.4\nvoice: ignored\n---\n- Books: call read_book.\n")
    assert books.pace() == {"speed": 0.75, "pause": 1.5, "sentence_pause": 0.4}
    assert books.spoken(page) == ("One. [pause:0.4s] Two! [pause:1.5s] 'Three,' said Peter. [pause:0.4s] Four?"
                                  " [pause:1.5s] Shall I keep going?")
    assert books.spoken(dict(page, jumped=True)).startswith("Page 2. [pause:1.5s] One.")

    (brain_dir / "skills" / "books.md").write_text("---\nspeed: 9\npause: -3\nsentence_pause: lots\n---\n- x\n")
    assert books.pace() == {"speed": 1.3, "pause": 0.0, "sentence_pause": 0.0}      # clamped, bad ones ignored
    assert books.spoken(page) == "One. Two!\n\n'Three,' said Peter. Four?\n\nShall I keep going?"
    (brain_dir / "skills" / "books.md").unlink()
    assert books.pace() == {"speed": 0.9, "pause": 0.8, "sentence_pause": 0.0}      # no file: the defaults
