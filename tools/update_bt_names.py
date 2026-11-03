"""Refreshes the Bluetooth name tables inside webui/index.html: company names,
device types (GAP appearance) and 16-bit service names.

The data is the Bluetooth SIG's assigned numbers, via Nordic Semiconductor's public
mirror (github.com/NordicSemiconductor/bluetooth-numbers-database).
Run now and then:  python3 tools/update_bt_names.py [folder with the three .json files]
"""
import json
import os
import re
import sys
import urllib.request

BASE = "https://raw.githubusercontent.com/NordicSemiconductor/bluetooth-numbers-database/master/v1/"
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PAGE = os.path.join(ROOT, "webui", "index.html")

# Legal suffixes add width, not meaning ("Bose Corporation" -> "Bose")
SUFFIX = re.compile(
    r"(,?\s+(Inc\.?|Incorporated|Corporation|Corp\.?|Co\.,?\s*Ltd\.?|Co\.?|Ltd\.?|Limited|LLC|L\.L\.C\.|GmbH|AG|"
    r"A/S|AB|AS|ASA|Oy|OY|S\.A\.|SA|S\.p\.A\.|B\.V\.|BV|N\.V\.|NV|SAS|S\.A\.S\.|KG|Pty|Pte|PLC|plc|"
    r"International|Industries|Electronics|Technologies|Technology|Holdings?|Group)\.?)+$")


def short(name):
    n = name.strip()
    for _ in range(4):
        m = SUFFIX.sub("", n).strip().rstrip(" ,&-").strip()
        if len(m) < 3 or m == n:  # "C Technologies" stays whole
            break
        n = m
    return n


def load(folder, name):
    if folder:
        return json.load(open(os.path.join(folder, name)))
    return json.load(urllib.request.urlopen(BASE + name))


def put(page, tag, body):
    a = page.index(f"/*{tag}:START*/") + len(f"/*{tag}:START*/")
    b = page.index(f"/*{tag}:END*/")
    return page[:a] + body + page[b:]


def main():
    folder = sys.argv[1] if len(sys.argv) > 1 else None
    companies = {int(x["code"]): short(x["name"]) for x in load(folder, "company_ids.json")}
    appearance = {}
    for x in load(folder, "gap_appearance.json"):
        subs = {int(s["value"]): s["name"] for s in x.get("subcategory", [])}
        appearance[int(x["category"])] = [x["name"], subs] if subs else [x["name"]]
    services = {}
    for x in load(folder, "service_uuids.json"):
        u = x["uuid"].upper()
        if len(u) == 4:
            services[int(u, 16)] = x["name"]
    j = lambda o: json.dumps(o, separators=(",", ":"), ensure_ascii=False)
    page = open(PAGE, encoding="utf-8").read()
    page = put(page, "COMPANIES", "const COMPANIES = " + j(companies) + ";")
    page = put(page, "BLENAMES", "const APPEARANCE = " + j(appearance) + ";\nconst SERVICES = " + j(services) + ";")
    open(PAGE, "w", encoding="utf-8").write(page)
    print(f"{len(companies)} companies, {len(appearance)} device types, {len(services)} services written")


main()
