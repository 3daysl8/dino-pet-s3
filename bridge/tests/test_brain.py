import json

import brain


def test_setup_copies_templates_and_migrates(tmp_path, monkeypatch):
    (tmp_path / "memory.json").write_text(json.dumps({"notes": ["Likes trains"]}))
    monkeypatch.setattr(brain, "DATA", str(tmp_path))
    monkeypatch.setattr(brain, "BRAIN", str(tmp_path / "brain"))
    brain.setup()
    assert (tmp_path / "brain" / "SOUL.md").exists()
    assert (tmp_path / "brain" / "people" / "sam.md").exists()
    assert "- Likes trains" in (tmp_path / "brain" / "MEMORY.md").read_text()
    assert not (tmp_path / "memory.json").exists() and (tmp_path / "memory.json.migrated").exists()


def test_memory_add_replace_remove(brain_dir):
    assert brain.memory_op("add", "memory", "We play I spy").startswith("ok")
    assert brain.memory_op("add", "memory", "we play i spy") == "ok (already noted)"
    assert brain.memory_op("replace", "memory", "We play I spy in the car", "I spy").startswith("ok")
    assert "in the car" in (brain_dir / "MEMORY.md").read_text()
    assert brain.memory_op("remove", "memory", old_text="nothing like this").startswith("error")
    assert brain.memory_op("remove", "memory", old_text="I spy").startswith("ok")
    assert (brain_dir / "MEMORY.md").read_text() == ""


def test_memory_cap_is_an_error(brain_dir):
    note = "x" * 150
    results = [brain.memory_op("add", "memory", f"{i} {note}") for i in range(20)]
    assert any(r.startswith("error: that would make the notes") for r in results)
    assert len((brain_dir / "MEMORY.md").read_text()) <= brain.MEMORY_CAP


def test_person_notes_keep_parents_part(brain_dir):
    before = (brain_dir / "people" / "sam.md").read_text()
    assert brain.memory_op("add", "Sam", "Favourite colour is green").startswith("ok")
    text = (brain_dir / "people" / "sam.md").read_text()
    assert text.startswith(before.rstrip()) and "## Deadlee's notes\n- Favourite colour is green" in text
    # Deadlee can't edit the parent's part
    assert brain.memory_op("remove", "sam", old_text="6 years old").startswith("error")
    brain.forget()
    assert "green" not in (brain_dir / "people" / "sam.md").read_text()
    assert "6 years old" in (brain_dir / "people" / "sam.md").read_text()


def test_person_alias_and_unknown(brain_dir):
    assert brain.memory_op("add", "Dad", "Likes coffee").startswith("ok")            # Alex (Dad)
    assert "Likes coffee" in (brain_dir / "people" / "alex.md").read_text()
    assert brain.memory_op("add", "Jack from school", "x").startswith("error")


def test_main_person_has_bigger_cap(brain_dir):
    assert brain.person_cap("sam") == brain.MAIN_PERSON_CAP > brain.person_cap("alex")


def test_family_add_only_appends(brain_dir):
    before = (brain_dir / "FAMILY.md").read_text()
    assert brain.family_add("Grandma visits on 24 December").startswith("ok")
    after = (brain_dir / "FAMILY.md").read_text()
    assert after.startswith(before.rstrip()) and "Grandma visits on 24 December" in after


def test_snapshot_has_family_people_and_notes(brain_dir):
    brain.memory_op("add", "memory", "Our secret word is banana")
    s = brain.snapshot("sam")
    assert "## Family" in s.knowledge and "About Sam (who is talking to you now)" in s.knowledge
    assert "banana" in s.knowledge and "weather" in s.skills.lower()
    assert "catalog" not in s.skills.lower()


def test_web_page_can_only_edit_brain_files(brain_dir):
    for bad in ("../pet.token", "skills/../SOUL.md", "cron.yaml", "gaps.jsonl", "voices/sam.npy",
                "skills/Bad Name.md", "/etc/passwd", "people/", ""):
        assert brain.file_cap(bad) == 0 and brain.read_file(bad) is None
        assert brain.write_file(bad, "x").startswith("error") and brain.delete_file(bad).startswith("error")
    paths = [f["path"] for f in brain.list_files()]
    assert paths[:3] == ["SOUL.md", "FAMILY.md", "MEMORY.md"]
    assert "people/sam.md" in paths and "skills/weather.md" in paths and "skills/catalog.md" in paths


def test_web_page_adds_edits_and_deletes_a_skill(brain_dir):
    assert brain.write_file("skills/maths.md", "- Maths: work sums out step by step.\r\n").startswith("ok")
    assert brain.read_file("skills/maths.md") == "- Maths: work sums out step by step.\n"
    assert "work sums out" in brain.snapshot().skills
    assert brain.write_file("skills/maths.md", "x" * (brain.SKILL_CAP + 1)).startswith("error: that is")
    assert "work sums out" in brain.read_file("skills/maths.md")          # a refused save changes nothing
    assert brain.delete_file("skills/maths.md") == "ok"
    assert "work sums out" not in brain.snapshot().skills
    assert brain.delete_file("SOUL.md").startswith("error") and (brain_dir / "SOUL.md").exists()


def test_gaps_are_redacted(brain_dir):
    brain.log_gap("Read Alex's email at 3pm on 12/10", "no email tool", "email access", ["Alex", "Dad"])
    rec = brain.read_jsonl("gaps.jsonl")[0]
    assert "Alex" not in rec["request"] and "12" not in rec["request"] and "[name]" in rec["request"]
