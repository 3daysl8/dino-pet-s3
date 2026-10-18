import json
import time

import brain
import routines


def test_cron_parse(brain_dir):
    (brain_dir / "cron.yaml").write_text("# jobs\nnightly: 2:30  # tidy\ntoday_card: \"06:30\"\nbad line\n")
    assert routines._read_cron() == {"nightly": "02:30", "today_card": "06:30"}


def test_digest_without_gaps_needs_no_model(brain_dir, monkeypatch):
    monkeypatch.setattr(routines, "_work", lambda *a, **k: (_ for _ in ()).throw(AssertionError("no LLM")))
    brain.log_event("fallback")
    brain.log_event("tool_error", "weather: ConnectTimeout")
    brain.log_event("tool_error", "weather: ConnectTimeout")
    d = routines.build_digest()
    assert "Nothing new this week." in d
    assert "Answered by the Claude backup: 1" in d and "weather 2" in d


def test_digest_with_gaps_uses_catalog(brain_dir, monkeypatch):
    brain.log_gap("read my email", "no email tool", "email access", [])
    brain.log_gap("old one", "x", "y", [])
    old = brain.read_jsonl("gaps.jsonl")
    old[1]["t"] = time.time() - 10 * 86400                      # outside the week
    (brain_dir / "gaps.jsonl").write_text("".join(json.dumps(g) + "\n" for g in old))
    seen = {}
    monkeypatch.setattr(routines, "_work", lambda system, user, n=0: seen.update(user=user) or "### Email (1 times)")
    d = routines.build_digest()
    assert "### Email (1 times)" in d and "read my email" in seen["user"] and "old one" not in seen["user"]
    assert "Maths helper" in seen["user"]                     # the catalogue is offered


def test_tidy_only_full_files(brain_dir, monkeypatch):
    for i in range(14):                                         # ~140 chars each: past 80% of 2,200
        assert brain.memory_op("add", "memory", f"note {i:02d} " + "about something we did together " * 4).startswith("ok")
    monkeypatch.setattr(routines, "_work", lambda *a, **k: '["We do lots together", "Note 13 kept"]')
    done = routines.tidy_notes()
    assert done == ["memory: 14 -> 2 notes"]
    assert (brain_dir / "MEMORY.md").read_text() == "- We do lots together\n- Note 13 kept\n"


def test_ritual_prompt_uses_card(brain_dir, monkeypatch):
    (brain_dir / "routines").mkdir()
    from datetime import datetime
    from prompt import TZ
    card = {"date": datetime.now(TZ).date().isoformat(), "weather": "Melbourne today: rain", "fact": "Owls..."}
    (brain_dir / "routines" / "today.json").write_text(json.dumps(card))
    seen = {}
    monkeypatch.setattr(routines, "_work", lambda system, user, n=0: seen.update(s=system, u=user) or "Morning!")
    assert routines.ritual("morning", brain.snapshot(), {"hunger": 2}, "sam") == "Morning!"
    assert "rain" in seen["u"] and "Speaker: sam" in seen["u"] and "## Family" in seen["s"]
