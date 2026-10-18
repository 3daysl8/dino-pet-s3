import numpy as np

import speaker
import tools


def unit(v):
    v = np.asarray(v, dtype=np.float32)
    return v / np.linalg.norm(v)


def test_identify_threshold_and_margin():
    prints = {"sam": unit([1, 0, 0]), "alex": unit([0, 1, 0])}
    assert speaker.identify(unit([0.95, 0.1, 0]), prints)[0] == "sam"
    assert speaker.identify(unit([0.1, 0.95, 0]), prints)[0] == "alex"
    assert speaker.identify(unit([0, 0, 1]), prints)[0] is None             # nobody we know
    assert speaker.identify(unit([1, 1, 0]), prints)[0] is None             # too close to call
    assert speaker.identify(None, prints) == (None, 0.0)
    assert speaker.identify(unit([1, 0, 0]), {}) == (None, 0.0)


def test_who_said_keeps_or_resets():
    prints = {"sam": unit([1, 0, 0])}
    assert speaker.who_said(unit([1, 0.05, 0]), prints)[0] == "sam"
    assert speaker.who_said(None, prints)[0] == speaker.KEEP                  # too short to judge
    assert speaker.who_said(unit([0.6, 0.8, 0]), prints)[0] == speaker.KEEP     # unclear (0.60)
    assert speaker.who_said(unit([0.5, 0.866, 0]), prints)[0] is None          # 0.50: someone else
    assert speaker.who_said(unit([0, 1, 0]), prints)[0] is None               # clearly someone else
    assert speaker.who_said(unit([1, 0, 0]), {})[0] is None                   # nobody enrolled


def test_enrolment_by_voice(brain_dir):
    e = speaker.Enrolment()
    assert not e.active(0)
    assert "favourite animal" in e.start("sam", 100)
    assert e.active(101)
    assert "too short" not in e.feed(None) and e.got == 0                  # asks again
    for i in range(speaker.ENROL_CLIPS):
        msg = e.feed(unit([1, 0.01 * i, 0]))
    assert "Got it, Sam" in msg and not e.active(200)
    prints = speaker.voices()
    assert set(prints) == {"sam"}
    assert speaker.identify(unit([1, 0.02, 0]), prints)[0] == "sam"
    assert speaker.forget("sam") == ["sam"] and speaker.voices() == {}


def test_enrolment_times_out():
    e = speaker.Enrolment()
    e.start("sam", 0)
    assert not e.active(6 * 60)


def test_enrol_tool_only_for_family(brain_dir):
    ctx = tools.Context()
    assert tools.run("enrol_voice", '{"name": "Jack"}', ctx).startswith("error") and ctx.enrol is None
    assert tools.run("enrol_voice", '{"name": "Dad"}', ctx).startswith("ok") and ctx.enrol == "alex"
