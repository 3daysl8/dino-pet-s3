from datetime import date

import family

TEXT = """## People
- Alex (Dad)
- Jo (Mum)
- Sam (6 years old)
<!-- - Ghost: 1 Jan -->
## Birthdays
- Mum: 21 Oct
- Sam: 5 Jan 2020
- Grandpa: 29 Feb
- Broken line
## Work
- Dad: office Tue, Thu
## Travel
- 2026-10-14 to 2026-10-16: Dad in Sydney (work)
- 2026-10-09: school concert
- 2027-01-20: Beach holiday
- 2026-13-01: bad date
## Added by Deadlee
- 2026-10-09: Sam says Grandma visits at Christmas
"""


def test_parse_sections():
    f = family.parse(TEXT)
    assert f.people == ["Alex (Dad)", "Jo (Mum)", "Sam (6 years old)"]
    assert ("Mum", 10, 21, None) in f.birthdays and ("Sam", 1, 5, 2020) in f.birthdays
    assert not any(b[0] == "Ghost" for b in f.birthdays)          # comments ignored
    assert len(f.travel) == 3
    assert any("Broken line" in p for p in f.problems) and any("bad date" in p for p in f.problems)


def test_view_counts_days():
    v = family.view(family.parse(TEXT), date(2026, 10, 9))
    assert "Mum: Wednesday 21 October, in 12 days" in v
    assert "Sam: Tuesday 5 January, in 88 days, turning 7" in v       # across the year end
    assert v.index("Mum:") < v.index("Sam:")                          # soonest first
    assert "school concert" in v.split("Happening now:")[1]              # today
    assert "Dad in Sydney (work): Wednesday 14 October, in 5 days to Friday 16 October, in 7 days" in v
    assert "Beach holiday" in v.split("Later:")[1]
    assert "Grandma visits at Christmas" in v


def test_away_now_and_tomorrow():
    v = family.view(family.parse(TEXT), date(2026, 10, 15))
    assert "Dad in Sydney (work), until tomorrow (Friday 16 October)" in v


def test_leap_day_birthday():
    v = family.view(family.parse(TEXT), date(2027, 2, 28))
    assert "Grandpa: tomorrow (Monday 1 March)" in v


def test_recent_birthday():
    v = family.view(family.parse(TEXT), date(2026, 1, 9))
    assert "Just had a birthday: Sam, 4 days ago, turned 6." in v
    assert "Just had a birthday" not in family.view(family.parse(TEXT), date(2026, 1, 20))


def test_speaker_marked():
    v = family.view(family.parse(TEXT), date(2026, 10, 9), speaker="sam")
    assert "Sam (the speaker):" in v


def test_append_added_keeps_everything():
    out = family.append_added(TEXT, "Swimming on Saturday", date(2026, 10, 10))
    assert out.startswith(TEXT.split("## Added by Deadlee")[0])
    assert "- 2026-10-09: Sam says Grandma visits at Christmas\n- 2026-10-10: Swimming on Saturday" in out
    fresh = family.append_added("## People\n- Alex\n", "Trip", date(2026, 10, 10))
    assert fresh.endswith("## Added by Deadlee\n- 2026-10-10: Trip\n")


def test_names_for_redaction():
    n = family.names(family.parse(TEXT))
    assert {"Alex", "Dad", "Jo", "Mum", "Sam", "Grandpa"} <= set(n)


def test_check_prints_counts_not_contents():
    out = family.check(TEXT)
    assert out.startswith("3 people, 3 birthdays, 3 trips/events, 1 work lines, 1 other sections")
    assert "1 line(s) in ## Birthdays" in out and "1 line(s) in ## Travel" in out
    assert "Broken line" not in out and "Alex" not in out and "Sydney" not in out.split("(")[0]
