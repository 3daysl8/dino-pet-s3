import json
import os

import brain
import llm
import prompt
import tools


def test_weather_place_comes_from_the_skill_file(brain_dir):
    assert tools._place() == tools.DEFAULT_PLACE                      # the template's
    (brain_dir / "skills" / "weather.md").write_text("---\nplace: Hobart  # home\nlat: -42.88\nlon: east\n---\n- Weather.\n")
    assert tools._place() == {"place": "Hobart", "lat": -42.88, "lon": 144.96}   # a bad number keeps the default


def test_no_canned_roars():
    soul = open(os.path.join(brain.TEMPLATE, "SOUL.md")).read()
    for line in prompt.LINES.values():
        assert "rawr" not in line.lower() and "roar" not in line.lower()
    assert all(l.startswith("Never") for l in soul.splitlines() if "rawr" in l.lower())
    assert prompt.tidy("Rawr! It's sunny") == "It's sunny"
    assert prompt.tidy("RAWR, roar... *stomps* tomorrow's cold") == "Tomorrow's cold"
    assert prompt.tidy("") == prompt.LINES["say_again"]


def test_fallback_system_has_no_brain():
    s = prompt.build_system("SOUL", private=False, knowledge="SECRET FAMILY", skills="SKILLS")
    assert "SECRET FAMILY" not in s and "SKILLS" not in s and "backup mode" in s
    assert "Speaker" not in prompt.state_text({"hunger": 3}, "sam", private=False)
    assert "Speaker: sam" in prompt.state_text({"hunger": 3}, "sam")


def test_claude_fallback_gets_nothing_private(brain_dir, monkeypatch):
    brain.memory_op("add", "memory", "Secret word banana")
    brain.memory_op("add", "sam", "Loves green")
    snap = brain.snapshot("sam")
    sent = {}

    class FakeMessages:
        def create(self, **kw):
            sent.update(kw)
            return type("R", (), {"stop_reason": "end_turn",
                                  "content": [type("B", (), {"type": "text", "text": "Hello"})()]})()

    def down(_body):
        raise llm.LocalError("down")

    monkeypatch.setattr(llm, "_post", down)
    monkeypatch.setattr(llm, "_claude", type("C", (), {"messages": FakeMessages()})())
    history = [{"role": "user", "content": "earlier private chat"}, {"role": "assistant", "content": "ok"}]
    ctx = tools.Context(speaker="sam")
    text, private = llm.reply(snap, history, "hi there", {"hunger": 2, "name": "Deadlee"}, ctx)
    assert text == "Hello" and private is False
    blob = json.dumps(sent).lower()
    # SOUL.md itself may name Sam (it's the persona); notes, people files, the
    # family view, history and who is speaking must not be there.
    for secret in ("banana", "green", "earlier private chat", "## family", "6 years old"):
        assert secret not in blob, secret
    assert "speaker" not in sent["messages"][0]["content"].lower()
    assert sent["messages"] == [{"role": "user", "content": sent["messages"][0]["content"]}]


def test_local_tool_loop_collects_actions(brain_dir, monkeypatch):
    replies = iter([
        {"content": "", "tool_calls": [{"id": "1", "function": {"name": "nightlight",
                                                                "arguments": '{"on": true, "colour": "blue"}'}}]},
        {"content": "Night light on, sleep tight."},
    ])
    seen = []
    monkeypatch.setattr(llm, "_post", lambda body: (seen.append(body), next(replies))[1])
    ctx = tools.Context()
    text, private = llm.reply(brain.snapshot(), [], "night light on, blue please", {}, ctx)
    assert private and text == "Night light on, sleep tight."
    assert ctx.actions == [{"type": "nightlight", "on": True, "colour": 2}]
    assert seen[0]["chat_template_kwargs"] == {"enable_thinking": False} and "tools" in seen[0]
    assert seen[1]["messages"][-1]["role"] == "tool"


def test_skipped_tool_is_retried_and_cant_is_logged(brain_dir, monkeypatch):
    bodies = []
    replies = iter([
        {"content": "Alarm set for seven!"},                                         # lied: no tool
        {"content": "", "tool_calls": [{"id": "1", "function": {"name": "set_alarm",
                                                                "arguments": '{"hour": 7, "minute": 0}'}}]},
        {"content": "Alarm set for seven."},
    ])
    monkeypatch.setattr(llm, "_post", lambda body: (bodies.append(body), next(replies))[1])
    ctx = tools.Context()
    text, _ = llm.reply(brain.snapshot(), [], "wake me up at 7", {}, ctx)
    assert ctx.actions == [{"type": "alarm", "h": 7, "m": 0, "days": 0}]
    assert "tool_choice" not in bodies[0] and bodies[1]["tool_choice"] == "required"

    monkeypatch.setattr(llm, "_post", lambda body: {"content": "Sorry, I can't read emails yet."})
    llm.reply(brain.snapshot(), [], "read my emails", {}, tools.Context())
    assert brain.read_jsonl("gaps.jsonl")[-1]["request"] == "read my emails"
    monkeypatch.setattr(llm, "_post", lambda body: {"content": "I can't wait for your birthday!"})
    llm.reply(brain.snapshot(), [], "it's my birthday soon", {}, tools.Context())
    assert len(brain.read_jsonl("gaps.jsonl")) == 1


def test_toy_action_validation():
    assert tools.toy_action("set_timer", {"minutes": 5, "label": "pasta!"})[0] == \
        {"type": "timer", "seconds": 300, "label": "pasta"}
    assert tools.toy_action("set_timer", {"minutes": 0})[0] is None
    assert tools.toy_action("set_alarm", {"hour": 7, "minute": 0, "repeat": "weekdays"})[0] == \
        {"type": "alarm", "h": 7, "m": 0, "days": 0x1F}
    assert tools.toy_action("set_alarm", {"hour": 25, "minute": 0})[0] is None
    assert tools.toy_action("nightlight", {"on": True, "colour": "pink"})[0] is None
    assert tools.toy_action("nightlight", {"on": True, "minutes": 45})[0] is None
    assert tools.toy_action("nightlight", {"on": True, "minutes": 0, "brightness": 7})[0] == \
        {"type": "nightlight", "on": True, "timer": 4, "level": 7}
    assert tools.toy_action("radio", {"action": "play", "station": "Triple R"})[0] == \
        {"type": "radio", "play": True, "station": "Triple R"}


def test_tool_run_never_raises(brain_dir):
    ctx = tools.Context()
    assert tools.run("set_timer", "not json", ctx).startswith("error")
    assert tools.run("nope", "{}", ctx).startswith("error")
    assert tools.run("cant_help", '{"request": "read email", "why": "no tool"}', ctx).startswith("noted")
    assert brain.read_jsonl("gaps.jsonl")[0]["request"] == "read email"
