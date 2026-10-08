#!/usr/bin/env python3
"""Regenerate components/gameday/teams.h from ESPN.

Run this at release time. The header is committed so firmware builds never
need network access.

    build_teams.py                 rebuild NFL and college football
    build_teams.py --league mlb    rebuild one league (repeatable)

Leagues not named are copied from the current teams.h unchanged, so adding a
sport can never move or rename a football row (Home Assistant's team select
lists teams in this order). Leagues come from kLeagues in leagues.h.

NFL and the other pro leagues come from the league team list. FBS membership is not exposed cleanly
by the teams endpoint (it returns FCS and D3 schools too), so the college
list is collected from a season's worth of FBS scoreboards and filtered by
conference id.

Men's college basketball's team list is Division I only, but it can trail a
school that just finished moving up; see mcbb_teams().
"""

import argparse
import json
import re
import sys
import urllib.request
from pathlib import Path

SITE_ROOT = "https://site.api.espn.com/apis/site/v2/sports"
STANDINGS_ROOT = "https://site.api.espn.com/apis/v2/sports"
D1_GROUP = "50"  # NCAA Division I, the parent of every D1 basketball conference
SITE = f"{SITE_ROOT}/football"

# ESPN conference group ids that make up the FBS.
FBS_GROUPS = {
    1: "ACC",
    4: "Big 12",
    5: "Big Ten",
    8: "SEC",
    9: "Pac-12",
    12: "Conference USA",
    15: "MAC",
    17: "Mountain West",
    18: "FBS Independents",
    37: "Sun Belt",
    151: "American",
}

COMPONENT = Path(__file__).resolve().parent.parent / "components" / "gameday"
OUT = COMPONENT / "teams.h"
LEAGUES_H = COMPONENT / "leagues.h"

# Fewest teams a fresh list may have before the run is treated as a bad read.
MIN_TEAMS = {"NFL": 32, "NCAA": 120, "MLB": 30, "NBA": 30, "NHL": 32, "WNBA": 13, "MLS": 29, "EPL": 20, "MCBB": 350}


# ESPN returns 403 to most unfamiliar agents but accepts ones that start with "curl/".
def get(url):
    req = urllib.request.Request(url, headers={"User-Agent": "curl/8.0 gameday-scoreboard"})
    with urllib.request.urlopen(req, timeout=30) as r:
        return json.load(r)


def nfl_teams():
    data = get(f"{SITE}/nfl/teams?limit=100")
    teams = []
    for entry in data["sports"][0]["leagues"][0]["teams"]:
        t = entry["team"]
        teams.append(("NFL", int(t["id"]), t["abbreviation"], t["displayName"], 0))
    return teams


def ncaa_teams():
    found = {}
    for week in range(1, 16):
        url = f"{SITE}/college-football/scoreboard?groups=80&limit=400&week={week}"
        try:
            data = get(url)
        except Exception as ex:  # a missing week is not fatal
            print(f"week {week}: {ex}", file=sys.stderr)
            continue
        for ev in data.get("events", []):
            for comp in ev["competitions"][0]["competitors"]:
                t = comp["team"]
                conf = int(t.get("conferenceId", 0) or 0)
                if conf not in FBS_GROUPS:
                    continue
                found[int(t["id"])] = ("NCAA", int(t["id"]), t["abbreviation"], t["displayName"], conf)
    return list(found.values())


def pro_teams(league, path):
    data = get(f"{SITE_ROOT}/{path}/teams?limit=1000")
    teams = []
    for entry in data["sports"][0]["leagues"][0]["teams"]:
        t = entry["team"]
        teams.append((league, int(t["id"]), t["abbreviation"], t["displayName"], 0))
    return teams


def mcbb_teams(path):
    """ESPN's men's college basketball list holds only Division I teams but
    can miss a school that just finished moving up. The D1 standings (one
    table per conference) name every member. A team in the standings but not
    in the list is kept only when its team endpoint places it under Division
    I, so a school that has since left D1 stays out."""
    found = {row[1]: row for row in pro_teams("MCBB", path)}
    standings = get(f"{STANDINGS_ROOT}/{path}/standings?group={D1_GROUP}")
    for conf in standings.get("children", []):
        for entry in conf.get("standings", {}).get("entries", []):
            tid = int(entry["team"]["id"])
            if tid in found:
                continue
            t = get(f"{SITE_ROOT}/{path}/teams/{tid}")["team"]
            parent = ((t.get("groups") or {}).get("parent") or {}).get("id")
            if str(parent) == D1_GROUP:
                print(f"MCBB: adding {t['displayName']} ({tid}), missing from the team list")
                found[tid] = ("MCBB", tid, t["abbreviation"], t["displayName"], 0)
    return list(found.values())


def cstr(s):
    return '"' + s.replace("\\", "\\\\").replace('"', '\\"') + '"'


def unescape(s):
    return re.sub(r"\\(.)", r"\1", s)


def league_table():
    """(enum name, ESPN path) in kLeagues order."""
    text = LEAGUES_H.read_text(encoding="utf-8")
    return re.findall(r'\{League::(\w+), "\w+", "([^"]+)",', text)


def current_rows():
    if not OUT.exists():
        return []
    rows = re.findall(r'\{League::(\w+), (\d+), "((?:[^"\\]|\\.)*)", "((?:[^"\\]|\\.)*)", (\d+)\}',
                      OUT.read_text(encoding="utf-8"))
    return [(lg, int(tid), unescape(a), unescape(n), int(g)) for lg, tid, a, n, g in rows]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--league", action="append", default=[], help="league to rebuild, e.g. mlb (repeatable)")
    ap.add_argument("--rewrite", action="store_true", help="rebuild nothing, only re-emit the header")
    args = ap.parse_args()
    table = league_table()
    paths = dict(table)
    rebuild = [] if args.rewrite else [x.upper() for x in args.league] or ["NFL", "NCAA"]
    for lg in rebuild:
        if lg not in paths:
            sys.exit(f"{lg} is not in kLeagues (leagues.h)")

    by_league = {}
    for row in current_rows():
        by_league.setdefault(row[0], []).append(row)
    for lg in rebuild:
        if lg == "NFL":
            fresh = nfl_teams()
        elif lg == "NCAA":
            fresh = ncaa_teams()
        elif lg == "MCBB":
            fresh = mcbb_teams(paths[lg])
        else:
            fresh = pro_teams(lg, paths[lg])
        print(f"{lg}: {len(fresh)} teams")
        if len(fresh) < MIN_TEAMS.get(lg, 1):
            sys.exit(f"expected at least {MIN_TEAMS[lg]} {lg} teams; is it the off season?")
        by_league[lg] = sorted(fresh, key=lambda r: r[3])

    lines = [
        "// Generated by scripts/build_teams.py. Do not edit by hand.",
        "#pragma once",
        "#include <cstddef>",
        "#include <cstdint>",
        "",
        '#include "leagues.h"',
        "",
        "namespace espn {",
        "",
        "struct Team {",
        "  League league;",
        "  uint32_t espn_id;",
        "  const char *abbr;",
        "  const char *name;",
        "  uint32_t group;  // conference group id for the college scoreboard, 0 otherwise",
        "};",
        "",
        "constexpr Team kTeams[] = {",
    ]
    for lg, _path in table:
        for league, tid, abbr, name, group in by_league.get(lg, []):
            lines.append(f"    {{League::{league}, {tid}, {cstr(abbr)}, {cstr(name)}, {group}}},")
    lines += [
        "};",
        "",
        "constexpr size_t kTeamCount = sizeof(kTeams) / sizeof(kTeams[0]);",
        "",
        "}  // namespace espn",
        "",
    ]
    OUT.write_text("\n".join(lines), encoding="utf-8", newline="\n")
    print(f"wrote {OUT}")


if __name__ == "__main__":
    main()
