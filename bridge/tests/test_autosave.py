import autosave
import brain
import llm


def test_fold_saves_with_limited_tools(brain_dir, monkeypatch):
    bodies = []
    replies = iter([
        {"content": "", "tool_calls": [
            {"id": "1", "function": {"name": "memory",
                                     "arguments": '{"action": "add", "target": "Sam", "content": "Best friend is Max"}'}},
            {"id": "2", "function": {"name": "family_add",
                                     "arguments": '{"note": "Camping at Lorne on 14 November 2026"}'}},
            {"id": "3", "function": {"name": "set_timer", "arguments": '{"minutes": 5}'}},
        ]},
        {"content": "done"},
    ])
    monkeypatch.setattr(llm, "_post", lambda body: (bodies.append(body), next(replies))[1])
    used = autosave.fold([
        {"speaker": "sam", "heard": "Max is my best friend", "reply": "Cool!"},
        {"speaker": None, "heard": "We're going camping at Lorne on the 14th of November", "reply": "Fun!"},
    ])
    assert used == ["memory", "family_add"]                       # set_timer refused
    assert {t["function"]["name"] for t in bodies[0]["tools"]} == {"memory", "family_add"}
    assert "Sam: Max is my best friend" in bodies[0]["messages"][1]["content"]
    assert "Best friend is Max" in (brain_dir / "people" / "sam.md").read_text()
    assert "Camping at Lorne on 14 November 2026" in (brain_dir / "FAMILY.md").read_text()
    assert brain.read_jsonl("events.jsonl")[-1]["kind"] == "autosave"


def test_fold_skips_empty_and_survives_model_down(brain_dir, monkeypatch):
    assert autosave.fold([{"heard": "", "reply": "I didn't catch that"}]) == []

    def down(_body):
        raise llm.LocalError("down")

    monkeypatch.setattr(llm, "_post", down)
    assert autosave.fold([{"heard": "hi", "reply": "hello"}]) == []
