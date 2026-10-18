"""Regenerate bridge/countries.json, the country facts Deadlee answers from (no network at
chat time). Source: https://github.com/mledoze/countries (ODbL 1.0), trimmed to what a
child asks about. Population is not in there; lookup.py asks the World Bank for it.

    python tools/gen_countries.py
"""
import json
import os
import urllib.request

SRC = "https://raw.githubusercontent.com/mledoze/countries/master/dist/countries.json"
OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "bridge", "countries.json")

data = json.load(urllib.request.urlopen(SRC, timeout=60))
names = {c["cca3"]: c["name"]["common"] for c in data}
out = []
for c in sorted(data, key=lambda c: c["name"]["common"]):
    out.append({
        "name": c["name"]["common"],
        "official": c["name"]["official"],
        "also": [a for a in c.get("altSpellings", []) if len(a) > 3],
        "code": c["cca3"],
        "capital": c.get("capital") or [],
        "region": c.get("region", ""),
        "subregion": c.get("subregion", ""),
        "languages": list((c.get("languages") or {}).values()),
        "money": [v["name"] for v in (c.get("currencies") or {}).values()],
        "area": c.get("area") if (c.get("area") or 0) > 0 else None,
        "borders": [names[b] for b in c.get("borders", []) if b in names],
        "landlocked": bool(c.get("landlocked")),
        "independent": bool(c.get("independent")),
    })
with open(OUT, "w", encoding="utf-8", newline="\n") as f:
    json.dump({"source": "mledoze/countries (ODbL 1.0)", "countries": out}, f, ensure_ascii=False,
              separators=(",", ":"))
print(len(out), "countries,", os.path.getsize(OUT) // 1024, "KB")
