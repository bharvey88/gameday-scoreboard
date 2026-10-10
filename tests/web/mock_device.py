"""Mock of the scoreboard's web server for the device page tests.

Serves firmware/web/bundle.js and app.css at /0.js and /0.css (where ESPHome's
web_server puts js_include and css_include), the ESPHome /events stream for
the entities the page builds controls from, and the gameday component's
routes with the response shapes from components/gameday/gameday.cpp:

  GET  /gameday/state    the document rebuild_state_() writes
  POST /gameday/set      only the keys in SET_KEYS; 400 when none match
  POST /gameday/action   400 without do=

Test hooks: POST /_reset with an optional JSON body
{"state": {...overrides...}, "fail": ["state", "set"]} puts the state back,
merges the overrides and makes the named routes answer 500. GET /_log lists
the /gameday/set and /gameday/action requests seen since the last reset.

Started by playwright.config.js. Run by hand with `python mock_device.py 8765`
and open http://127.0.0.1:8765/.
"""
import copy
import json
import os
import re
import sys
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import parse_qs, urlparse

REPO = Path(__file__).resolve().parents[2]
WEB = Path(os.environ.get("GAMEDAY_WEB", REPO / "firmware" / "web"))

# Same tables the firmware and scripts/build_web.py read.
TEAM_RE = re.compile(r'\{League::([A-Z]+),\s*(\d+),\s*"([^"]*)",\s*"([^"]*)",\s*(\d+)\}')
LEAGUE_RE = re.compile(r'\{League::[A-Z]+,\s*"([a-z]+)",')
TZ_RE = re.compile(r'\{"([^"]+)",\s*"([^"]+)",\s*"([^"]+)"\}')
TEAMS = {}
for lg, tid, abbr, name, _ in TEAM_RE.findall((REPO / "components/gameday/teams.h").read_text(encoding="utf-8")):
    TEAMS[(lg.lower(), int(tid))] = (abbr, name)
# kLeagues order, which is the order the firmware lists live leagues in.
LEAGUES = LEAGUE_RE.findall((REPO / "components/gameday/leagues.h").read_text(encoding="utf-8"))
FAV_MAX = 16
TZ_TEXT = "\n".join(ln for ln in (REPO / "components/gameday/timezones.h").read_text(encoding="utf-8").splitlines()
                    if not ln.lstrip().startswith("//"))
TZS = [name for name, _posix, _iana in TZ_RE.findall(TZ_TEXT)]

SET_KEYS = ("team", "mode", "rotate", "fav1", "fav2", "fav3", "fav4", "tz", "tzauto", "down", "play", "odds",
            "opp", "panels", "bootaddr", "lockon", "release", "collide", "favs", "today", "live")


def team(lg, tid):
    """put_team(): league, id, abbr and name, or an empty object."""
    if (lg, tid) not in TEAMS:
        return {}
    abbr, name = TEAMS[(lg, tid)]
    return {"l": lg, "id": tid, "abbr": abbr, "name": name}


INITIAL = {
    "name": "gameday-2f6a70", "version": "v1.5.4", "setup": True, "panels": 2,
    "team": team("nfl", 6),
    "mode": 0, "rotate": 1, "lockon": 15, "release": 30, "collide": 0,
    "favs": [team("nfl", 6), team("nfl", 34), team("ncaa", 145), {}],
    "favmax": FAV_MAX, "today": False, "live": ["nfl", "ncaa"], "live_auto": True,
    "tz": 1, "tz_name": "US Central", "tz_auto": True,
    "down": True, "play": True, "odds": True, "opp": True, "bootaddr": True,
    "misses": 0, "stale_s": 4, "fallback": False,
    "game": {"s": "PRE", "l": "nfl", "ta": "DAL", "ti": 6, "ts": 0, "oa": "PHI", "oi": 21, "os": 0,
             "tr": "1-0", "or": "0-1", "p": 0, "tt": 3, "ot": 3, "rz": False, "tv": "FOX",
             "venue": "AT&T Stadium", "odds": "DAL -3", "ou": "47.5", "detail": "Sun 3:25 PM",
             "kick": 1792531500, "lp": "", "dd": "", "c": "", "d": "", "k": "Sun 3:25 PM",
             "tc": "041E42", "oc": "004C54", "id": "401772001"},
    "status": "DAL vs PHI | Sun 3:25 PM | on FOX",
    "splash": "", "splash_color": "000000",
    "next": [{"id": "401772001", "kick": 1792531500, "oi": 21, "oa": "PHI", "on": "Eagles", "home": True,
              "neutral": False, "tv": "FOX"},
             {"id": "401772002", "kick": 1793136300, "oi": 28, "oa": "WSH", "on": "Commanders", "home": False,
              "neutral": False, "tv": "CBS"},
             {"id": "401772003", "kick": 1793741100, "oi": 34, "oa": "HOU", "on": "Texans", "home": True,
              "neutral": False, "tv": "NBC"}],
}

# ESPHome 2026.x state events (detail=all shape) for the entities app.js uses.
ENTITIES = [
    {"id": "switch/Power", "domain": "switch", "name": "Power", "state": "ON", "value": True},
    {"id": "number/Brightness", "domain": "number", "name": "Brightness", "state": "128", "value": "128",
     "min_value": "1", "max_value": "255", "step": "1"},
    {"id": "number/Scroll Speed", "domain": "number", "name": "Scroll Speed", "state": "5", "value": "5",
     "min_value": "1", "max_value": "10", "step": "1"},
    {"id": "select/Select Page", "domain": "select", "name": "Select Page", "state": "Scoreboard",
     "value": "Scoreboard", "option": ["Scoreboard", "Clock"]},
    {"id": "text_sensor/Game Status", "domain": "text_sensor", "name": "Game Status",
     "state": INITIAL["status"], "value": INITIAL["status"]},
    {"id": "update/Firmware", "domain": "update", "name": "Firmware", "state": "NO UPDATE", "value": "1.5.4",
     "current_version": "1.5.4", "summary": "", "release_url": ""},
    {"id": "text_sensor/IP", "domain": "text_sensor", "name": "IP", "state": "10.10.10.97", "value": "10.10.10.97"},
    {"id": "button/Refresh Now", "domain": "button", "name": "Refresh Now", "state": ""},
    {"id": "button/Reboot", "domain": "button", "name": "Reboot", "state": ""},
]
FIRMWARE = next(e for e in ENTITIES if e["name"] == "Firmware")

STATE = copy.deepcopy(INITIAL)
FAIL = set()
LOG = []


def parse_team_ref(v):
    lg, _, tid = v.partition(":")
    if lg not in LEAGUES or not tid.isdigit() or int(tid) == 0:
        return None
    return lg, int(tid)


def set_favs(refs):
    """set_favorites_(): the list in order, then empty slots up to the four remote slots."""
    favs = [team(*r) for r in refs[:FAV_MAX]]
    STATE["favs"] = favs + [{}] * (4 - len(favs))


def live_auto_leagues():
    """live_leagues_() with no mask: the saved team's league and the favorites'."""
    used = {t["l"] for t in [STATE["team"]] + STATE["favs"] if t}
    return [lg for lg in LEAGUES if lg in used] or ["nfl"]


def apply_set(kv):
    """GamedayComponent::apply_set_ for the keys the page sends."""
    for k, v in kv:
        if k == "team":
            ref = parse_team_ref(v)
            if ref and ref in TEAMS:
                STATE["team"] = team(*ref)
        elif k == "mode":
            m = int(v) if v.lstrip("-").isdigit() else -1
            if 0 <= m <= 5:
                STATE["mode"] = m
        elif k == "rotate":
            STATE["rotate"] = min(30, max(1, int(v or 0)))
        elif k == "lockon":
            STATE["lockon"] = min(120, max(5, int(v or 0)))
        elif k == "release":
            STATE["release"] = min(3600, max(30, int(v or 0)))
        elif k == "collide":
            STATE["collide"] = 1 if v == "1" else 0
        elif k == "favs":
            refs = []
            for part in v.split(",") if v else []:
                ref = parse_team_ref(part)
                if ref in TEAMS and ref not in refs:
                    refs.append(ref)
            set_favs(refs)
        elif k.startswith("fav"):
            ref = parse_team_ref(v)
            if (ref is None or ref not in TEAMS) and v not in ("none", ""):
                continue
            favs = STATE["favs"]
            favs[int(k[3]) - 1] = team(*ref) if ref else {}
        elif k == "today":
            STATE["today"] = v == "1"
        elif k == "live":
            keys = [lg for lg in LEAGUES if lg in v.split(",")]
            STATE["live_auto"] = not keys
            STATE["live"] = keys or live_auto_leagues()
        elif k == "tz":
            i = int(v) if v.isdigit() else -1
            if 0 <= i < len(TZS):
                STATE["tz"], STATE["tz_name"] = i, TZS[i]
        elif k == "tzauto":
            STATE["tz_auto"] = v == "1"
        elif k in ("down", "play", "odds", "opp", "bootaddr"):
            STATE[k] = v == "1"
        elif k == "panels":
            STATE["panels"] = int(v)


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass

    def _send(self, code, ctype, body):
        b = body.encode() if isinstance(body, str) else body
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(b)))
        self.end_headers()
        self.wfile.write(b)

    def _json(self, code, doc):
        self._send(code, "application/json", json.dumps(doc))

    def do_GET(self):
        u = urlparse(self.path)
        if u.path == "/":
            html = ("<!DOCTYPE html><html><head><meta charset=UTF-8><link rel=icon href=data:>"
                    "<link rel=stylesheet href=/0.css></head><body><script src=/0.js></script></body></html>")
            return self._send(200, "text/html", html)
        if u.path == "/0.js":
            return self._send(200, "text/javascript", (WEB / "bundle.js").read_bytes())
        if u.path == "/0.css":
            return self._send(200, "text/css", (WEB / "app.css").read_bytes())
        if u.path == "/gameday/state":
            if "state" in FAIL:
                return self._send(500, "text/plain", "")
            return self._json(200, STATE)
        if u.path == "/update/Firmware":
            return self._json(200, FIRMWARE)
        if u.path == "/events":
            return self._events()
        if u.path == "/_log":
            return self._json(200, LOG)
        self._send(404, "text/plain", "Not Found")

    def _events(self):
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.send_header("Cache-Control", "no-cache")
        self.end_headers()
        try:
            ping = {"title": "Game Day Scoreboard", "comment": STATE["version"]}
            self.wfile.write(("event: ping\ndata: " + json.dumps(ping) + "\n\n").encode())
            for e in ENTITIES:
                self.wfile.write(("event: state\ndata: " + json.dumps(e) + "\n\n").encode())
            self.wfile.flush()
            while True:
                time.sleep(5)
                self.wfile.write(b"event: ping\ndata: \n\n")
                self.wfile.flush()
        except (BrokenPipeError, ConnectionResetError, ConnectionAbortedError):
            return

    def do_POST(self):
        u = urlparse(self.path)
        pairs = parse_qs(u.query, keep_blank_values=True)
        if u.path == "/_reset":
            n = int(self.headers.get("Content-Length") or 0)
            body = json.loads(self.rfile.read(n) or b"{}")
            LOG.clear()
            STATE.clear()
            STATE.update(copy.deepcopy(INITIAL))
            STATE.update(body.get("state") or {})
            FAIL.clear()
            FAIL.update(body.get("fail") or [])
            return self._json(200, {"ok": True})
        if u.path == "/gameday/set":
            q = {k: v[0] for k, v in pairs.items()}
            LOG.append({"path": u.path, "q": q})
            if "set" in FAIL:
                return self._send(500, "text/plain", "")
            kv = [(k, q[k]) for k in SET_KEYS if k in q]
            if not kv:
                return self._json(400, {"error": "nothing to set"})
            apply_set(kv)
            return self._json(200, {"ok": True})
        if u.path == "/gameday/action":
            q = {k: v[0] for k, v in pairs.items()}
            LOG.append({"path": u.path, "q": q})
            if not q.get("do"):
                return self._json(400, {"error": "do is required"})
            return self._json(200, {"ok": True})
        if u.path.startswith("/gameday/"):
            return self._json(404, {"error": "unknown route"})
        if u.path.split("/")[1] in ("switch", "number", "select", "button", "update"):
            return self._send(200, "text/plain", "")
        self._send(404, "text/plain", "Not Found")


if __name__ == "__main__":
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8765
    ThreadingHTTPServer(("127.0.0.1", port), Handler).serve_forever()
