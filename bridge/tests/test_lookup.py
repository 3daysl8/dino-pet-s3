from datetime import date, timedelta

import httpx
import pytest

import lookup
import tools


@pytest.fixture
def web(monkeypatch):
    """Canned answers by a piece of the address; every call is recorded."""
    calls, answers = [], {}

    def fake(url, params=None, headers=None, timeout=8.0):
        calls.append((url, params))
        for piece, answer in answers.items():
            if piece in url:
                if isinstance(answer, Exception):
                    raise answer
                return answer(params) if callable(answer) else answer
        return None
    monkeypatch.setattr(lookup, "_json", fake)
    lookup._holiday_years.clear()
    lookup._people.clear()
    lookup._told.clear()
    return calls, answers


def test_encyclopedia_prefers_the_simple_one(web):
    calls, answers = web
    answers["simple.wikipedia.org/w/rest.php"] = {"pages": [{"key": "Blue_whale"}]}
    answers["simple.wikipedia.org/api/rest_v1/page/summary/Blue_whale"] = {
        "title": "Blue whale", "extract": "The blue whale is the biggest animal that has ever lived. " * 30}
    out = lookup.encyclopedia("  blue   whale ")
    assert out.startswith("Blue whale: The blue whale is the biggest") and len(out) < 640 and out.endswith(".")
    assert calls[0][1] == {"q": "blue whale", "limit": 1} and not any("en.wikipedia" in u for u, _ in calls)


def test_encyclopedia_falls_back_then_gives_up(web):
    calls, answers = web
    answers["simple.wikipedia.org/w/rest.php"] = {"pages": []}
    answers["en.wikipedia.org/w/rest.php"] = {"pages": [{"key": "Quokka"}]}
    answers["en.wikipedia.org/api/rest_v1/page/summary/Quokka"] = {
        "title": "Quokka", "extract": "The quokka is a small wallaby about the size of a cat."}
    assert lookup.encyclopedia("quokka").startswith("Quokka: The quokka is a small wallaby")
    answers["en.wikipedia.org/w/rest.php"] = {"pages": []}
    assert lookup.encyclopedia("zzqq").startswith("Nothing found")
    assert lookup.encyclopedia("").startswith("error")


SIMPLE_BAT = {"query": {"pages": {"8928": {"extract": (
    "\n=== Pronunciation ===\nenPR: bat\n\n\n== Noun ==\n\n(countable) A bat is a small, furry animal that "
    "has wings and can fly.\n(countable) A bat is a special stick used to hit the ball.\nThird line.\n\n\n"
    "== Verb ==\n\nIf you bat, you swing something.\nShe batted her eyelashes shyly.\n")}}}}


def test_dictionary_spells_in_code_and_reads_simple_meanings(web):
    calls, answers = web
    answers["simple.wiktionary.org"] = SIMPLE_BAT
    out = lookup.dictionary("Bat!")
    assert out.startswith("Spelling: B-A-T (3 letters). Meaning: (noun) A bat is a small, furry animal")
    assert "(noun) A bat is a special stick" in out and "(verb) If you bat" in out
    assert "Third line" not in out and "enPR" not in out and "countable" not in out and "eyelashes" not in out
    assert lookup.spell("ice cream") == "I-C-E, then C-R-E-A-M"


def test_dictionary_falls_back_and_never_guesses(web):
    calls, answers = web
    answers["simple.wiktionary.org"] = {"query": {"pages": {"-1": {"missing": ""}}}}
    answers["en.wiktionary.org"] = {"en": [{"partOfSpeech": "Adjective", "definitions": [
        {"definition": "Extremely <a href=\"/wiki/large\">large</a>; huge &amp; vast."}, {"definition": ""}]}]}
    assert lookup.dictionary("enormous") == ("Spelling: E-N-O-R-M-O-U-S (8 letters). Meaning: "
                                             "(adjective) Extremely large; huge & vast.")
    answers["en.wiktionary.org"] = None
    out = lookup.dictionary("flibberty")
    assert "isn't in the dictionary" in out and "F-L-I" not in out
    assert lookup.dictionary("123").startswith("error")


def test_holidays_are_for_home_with_days_counted(web):
    calls, answers = web
    today = date(2026, 10, 10)
    answers["PublicHolidays/2026/AU"] = [
        {"date": "2026-09-25", "localName": "Friday before AFL Grand Final", "global": False, "counties": ["AU-VIC"]},
        {"date": "2026-11-02", "localName": "Recreation Day", "global": False, "counties": ["AU-TAS"]},
        {"date": "2026-11-03", "localName": "Melbourne Cup", "global": False, "counties": ["AU-VIC"]},
        {"date": "2026-12-25", "localName": "Christmas Day", "global": True, "counties": None}]
    answers["PublicHolidays/2027/AU"] = [
        {"date": "2027-01-01", "localName": "New Year's Day", "global": True, "counties": None}]
    out = lookup.holidays(today)
    assert "- Melbourne Cup: Tuesday 3 November, in 24 days" in out and "- New Year's Day:" in out
    assert "Recreation Day" not in out and "AFL" not in out
    lookup.holidays(today)
    assert len(calls) == 2                                   # each year is fetched once a day
    lookup._holiday_years.clear()
    answers["PublicHolidays/2027/AU"] = httpx.ConnectError("down")
    assert "Christmas Day" in lookup.holidays(today)         # next year failing doesn't lose this year


def test_jokes_are_filtered_and_not_repeated(web):
    calls, answers = web
    answers["icanhazdadjoke.com/search"] = {"results": [
        {"joke": "A man walks into a bar and orders a beer."},
        {"joke": "What kind of dinosaur loves to sleep? A stega-snore-us."}]}
    assert lookup.joke("dinosaur!") == "What kind of dinosaur loves to sleep? A stega-snore-us."
    random_jokes = iter([{"joke": "My wife says I never listen."}, {"joke": "x" * 300},
                         {"joke": "Why did the scarecrow win a prize? He was outstanding in his field."}])
    answers["icanhazdadjoke.com/"] = lambda params: next(random_jokes)
    out = lookup.joke("dinosaur")                            # the only dinosaur joke was just told
    assert out == ("(No joke about dinosaur, so here is a different one.) "
                   "Why did the scarecrow win a prize? He was outstanding in his field.")
    answers["icanhazdadjoke.com/"] = {"joke": "I love my beer."}
    answers["icanhazdadjoke.com/search"] = {"results": []}
    assert lookup.joke().startswith("No joke came back")
    for ok in ("Hello, the barn door and the warm button.", "A brave brain."):
        assert not lookup.NOT_FOR_KIDS.search(ok), ok


def test_country_facts_come_from_the_bundled_file(web):
    calls, answers = web
    answers["api.worldbank.org"] = [{}, [{"value": 123366734, "date": "2025"}]]
    out = lookup.country("japan")
    assert out.startswith("Japan is in Eastern Asia. Capital: Tokyo. Languages: Japanese. Money: Japanese yen.")
    assert "About 123 million people live there (2025)." in out and "It has no land neighbours." in out
    assert "JPN" in calls[0][0]
    answers["api.worldbank.org"] = httpx.ConnectError("down")
    fr = lookup.country("Frannce")                           # a near miss still finds it
    assert fr.startswith("France is in Western Europe. Capital: Paris.") and "people live" not in fr
    assert "Neighbours:" in fr and "Germany" in fr
    assert lookup.find_country("the USA")["name"] == "United States"
    assert lookup.find_country("UK")["name"] == "United Kingdom"
    assert lookup.country("Narnia").startswith("I don't have a country")
    assert lookup.country("New Zealand").startswith("New Zealand is in Oceania.")
    assert lookup._big(1417000000) == "1.4 billion" and lookup._big(5300000) == "5.3 million"
    assert lookup._big(26000000) == "26 million" and lookup._big(800) == "800"


def forecast(today: date) -> dict:
    days = [today + timedelta(days=i) for i in range(7)]
    return {"current": {"weather_code": 0, "temperature_2m": 18.2, "apparent_temperature": 17.0, "wind_speed_10m": 9.6},
            "daily": {"time": [d.isoformat() for d in days], "weather_code": [3, 61, 0, 0, 0, 0, 95],
                      "temperature_2m_max": [20.1, 15.4, 22, 23, 24, 25, 26],
                      "temperature_2m_min": [12.7, 9.2, 10, 11, 12, 13, 14],
                      "precipitation_probability_max": [21, 80, 0, 0, 0, 0, 60],
                      "uv_index_max": [5.2, 3.1, 6, 6, 6, 6, 7],
                      "sunrise": [f"{d.isoformat()}T06:32" for d in days],
                      "sunset": [f"{d.isoformat()}T19:41" for d in days]}}


def test_weather_days_of_the_week_and_the_week(brain_dir, monkeypatch):
    today = date(2026, 10, 10)                               # a Saturday
    monkeypatch.setattr(tools, "_fetch_forecast", lambda p: forecast(today))
    assert tools.weather("today") == ("Melbourne today: cloudy, top of 20, low of 13 degrees, 21% chance of rain, "
                                      "UV 5. Sunrise 6:32 am, sunset 7:41 pm.")
    assert tools.weather("Tomorrow").startswith("Melbourne tomorrow: light rain, top of 15")
    assert tools.weather("sunday").startswith("Melbourne tomorrow (Sunday 11 October): light rain")
    assert tools.weather("friday").startswith("Melbourne Friday 16 October, in 6 days: thunderstorms, top of 26")
    assert tools.weather("saturday").startswith("Melbourne today (Saturday 10 October): cloudy")
    week = tools.weather("week").splitlines()
    assert week[0] == "Melbourne, the next 7 days:" and len(week) == 8
    assert week[1] == "- Today: cloudy, top of 20, low of 13, 21% chance of rain" and week[2].startswith("- Sunday:")
    assert tools.weather("now").startswith("Melbourne now: clear, 18 degrees")


def test_new_tools_are_offered_and_dispatched(brain_dir, web):
    calls, answers = web
    names = {t["function"]["name"] for t in tools.TOOLS}
    assert {"encyclopedia", "dictionary", "public_holidays", "joke", "country"} <= names
    when = next(t for t in tools.TOOLS if t["function"]["name"] == "weather")["function"]["parameters"]
    assert {"saturday", "week"} <= set(when["properties"]["when"]["enum"])
    ctx = tools.Context()
    answers["simple.wiktionary.org"] = SIMPLE_BAT
    assert tools.run("dictionary", '{"word": "bat"}', ctx).startswith("Spelling: B-A-T")
    assert tools.run("country", '{"name": "Australia"}', ctx).startswith("Australia is in")
    answers["icanhazdadjoke.com/"] = httpx.ConnectError("down")
    assert tools.run("joke", "{}", ctx) == "error: that didn't work just now (network problem)"
    assert ctx.used == ["dictionary", "country", "joke"]


def test_talking_speed_follows_the_tools_used(brain_dir):
    import brain
    assert brain.front_matter("lookup") == {"joke": "0.9", "country": "0.9", "encyclopedia": "0.9"}
    assert brain.front_matter("toy") == {} and brain.front_matter("nope") == {}
    assert tools.speed_for([], 1.1) == 1.1 and tools.speed_for(["weather", "memory"], 1.1) == 1.1
    assert tools.speed_for(["joke"], 1.1) == 0.9 and tools.speed_for(["weather", "country"], 1.1) == 0.9
    (brain_dir / "skills" / "lookup.md").write_text(
        "---\n# a note: ignored\njoke: 0.8   # slow\ncountry: 2\nencyclopedia: fast\ndictionary: 0.95\n---\n- x\n")
    assert tools.speed_for(["joke", "dictionary"], 1.1) == 0.8          # the slowest that applies
    assert tools.speed_for(["country"], 1.1) == 1.3                      # kept in range
    assert tools.speed_for(["encyclopedia"], 1.1) == 1.1                 # not a number: everyday speed
    assert "joke: 0.8" not in brain.snapshot().skills                    # settings aren't sent to the model
