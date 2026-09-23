"""Mock of the scoreboard's web server for page tests: the stock ESPHome
entity endpoints for the entities that stay, plus the /gameday routes.

Started by playwright.config.js. Run by hand with `python mock_device.py 8765`
and open http://localhost:8765/. GET /_log lists the settings posts it saw,
POST /_reset puts the state back.
"""
import copy
import json
import os
import sys
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import parse_qs, urlparse

ROOT = Path(os.environ.get("GAMEDAY_WEB", Path(__file__).resolve().parents[2] / "firmware" / "web"))
LOG = []

INITIAL = {
    "name": "gameday-2f6a70", "version": "v1.5.0", "setup": True, "panels": 2,
    "team": {"l": "nfl", "id": 6, "abbr": "DAL", "name": "Dallas Cowboys"},
    "mode": 0, "rotate": 5, "lockon": 15, "release": 30, "collide": 0, "shown": 0, "locked": False,
    "favs": [{"l": "nfl", "id": 6, "abbr": "DAL", "name": "Dallas Cowboys"}, {"l": "nfl", "id": 34, "abbr": "HOU", "name": "Houston Texans"},
             {"l": "ncaa", "id": 145, "abbr": "MISS", "name": "Ole Miss Rebels"}, {}],
    "tz": 1, "tz_name": "Central", "tz_auto": True,
    "down": True, "play": True, "odds": True, "opp": True, "bootaddr": True, "misses": 0, "stale_s": 4,
    "fallback": False, "fallback_mode": "next_game",
    "status": "DAL vs PHI | Sun 3:25 PM | on FOX",
    "splash": "", "splash_color": "000000",
    "next": [{"id": "1", "kick": 1757971500, "oi": 21, "oa": "PHI", "on": "Eagles", "home": True, "neutral": False, "tv": "FOX"},
             {"id": "2", "kick": 1758576300, "oi": 28, "oa": "WSH", "on": "Commanders", "home": True, "neutral": False, "tv": "FOX"}],
    "game": {"id": "1", "s": "PRE", "l": "nfl", "ta": "DAL", "ti": 6, "ts": 0, "oa": "PHI", "oi": 21, "os": 0,
             "tr": "1-0", "or": "0-1", "p": 0, "tt": 3, "ot": 3, "rz": False, "tv": "FOX",
             "venue": "AT&T Stadium", "odds": "DAL -3", "ou": "47.5", "detail": "Sun 3:25 PM",
             "kick": 1757971500, "lp": "", "dd": "", "c": "", "d": "", "k": "Sun 3:25 PM",
             "tc": "041E42", "oc": "004C54"},
}
STATE = copy.deepcopy(INITIAL)

FAV_NEXT = [
    {"slot": 1, "t": {"l": "nfl", "id": 6, "abbr": "DAL", "name": "Dallas Cowboys"}, "id": "1", "s": "PRE", "kick": 1757971500, "oi": 21, "oa": "PHI", "ts": 0, "os": 0, "tv": "FOX", "detail": "Sun 3:25 PM"},
    {"slot": 2, "t": {"l": "nfl", "id": 34, "abbr": "HOU", "name": "Houston Texans"}, "id": "5", "s": "IN", "kick": 1757960000, "oi": 30, "oa": "JAX", "ts": 14, "os": 10, "tv": "CBS", "detail": "8:12 - 2nd"},
]

ENTITIES = [
    {"id": "switch/Power", "domain": "switch", "name": "Power", "state": "ON", "value": True},
    {"id": "number/Brightness", "domain": "number", "name": "Brightness", "state": "128", "value": 128, "min_value": 1, "max_value": 255, "step": 1},
    {"id": "number/Scroll Speed", "domain": "number", "name": "Scroll Speed", "state": "5", "value": 5, "min_value": 1, "max_value": 10, "step": 1},
    {"id": "select/Select Page", "domain": "select", "name": "Select Page", "state": "Scoreboard", "value": "Scoreboard", "option": ["Scoreboard", "Clock"]},
    {"id": "text_sensor/Game Status", "domain": "text_sensor", "name": "Game Status", "state": INITIAL["status"], "value": INITIAL["status"]},
    {"id": "update/Firmware", "domain": "update", "name": "Firmware", "state": "NO UPDATE", "value": "1.5.0", "current_version": "1.5.0", "summary": "", "release_url": ""},
    {"id": "text_sensor/IP", "domain": "text_sensor", "name": "IP", "state": "10.10.10.97", "value": "10.10.10.97"},
    {"id": "button/Refresh Now", "domain": "button", "name": "Refresh Now", "state": ""},
    {"id": "button/Reboot", "domain": "button", "name": "Reboot", "state": ""},
]


def apply_set(q):
    """Mirrors GamedayComponent::apply_set_ closely enough for the page."""
    for k, v in q.items():
        if k == "team":
            lg, tid = v.split(":")
            STATE["team"] = {"l": lg, "id": int(tid), "abbr": "X", "name": "Team " + tid}
        elif k == "mode":
            STATE["mode"] = int(v)
        elif k in ("rotate", "lockon", "release", "collide", "panels", "tz"):
            STATE[k] = int(v)
        elif k.startswith("fav") and len(k) == 4:
            i = int(k[3]) - 1
            STATE["favs"][i] = {} if v == "none" else {"l": v.split(":")[0], "id": int(v.split(":")[1])}
        elif k == "tzauto":
            STATE["tz_auto"] = v == "1"
        elif k in ("down", "play", "odds", "opp", "bootaddr"):
            STATE[k] = v == "1"
        elif k == "fallback":
            if v in ("my_team", "1"):
                STATE["fallback_mode"] = "my_team"
            elif v in ("next_game", "0"):
                STATE["fallback_mode"] = "next_game"


class H(BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass

    def _send(self, code, ctype, body):
        b = body.encode() if isinstance(body, str) else body
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(b)))
        self.end_headers()
        self.wfile.write(b)

    def do_GET(self):
        u = urlparse(self.path)
        if u.path == "/":
            html = "<!doctype html><html><head><meta charset=utf-8><link rel=stylesheet href=/app.css></head><body><script src=/bundle.js></script></body></html>"
            return self._send(200, "text/html", html)
        if u.path == "/bundle.js":
            return self._send(200, "application/javascript", (ROOT / "bundle.js").read_bytes())
        if u.path == "/app.css":
            return self._send(200, "text/css", (ROOT / "app.css").read_bytes())
        if u.path == "/gameday/state":
            doc = copy.deepcopy(STATE)
            if STATE["mode"] == 4:
                doc["next"] = FAV_NEXT
                doc["shown"] = 2
                doc["locked"] = True
            return self._send(200, "application/json", json.dumps(doc))
        if u.path == "/update/Firmware":
            return self._send(200, "application/json", json.dumps(ENTITIES[5]))
        if u.path == "/events":
            self.send_response(200)
            self.send_header("Content-Type", "text/event-stream")
            self.send_header("Cache-Control", "no-cache")
            self.end_headers()
            try:
                self.wfile.write(("event: ping\ndata: " + json.dumps({"title": "Game Day Scoreboard", "comment": STATE["version"]}) + "\n\n").encode())
                for e in ENTITIES:
                    self.wfile.write(("event: state\ndata: " + json.dumps(e) + "\n\n").encode())
                self.wfile.flush()
                while True:
                    time.sleep(5)
                    self.wfile.write(b"event: ping\ndata: \n\n")
                    self.wfile.flush()
            except (BrokenPipeError, ConnectionResetError, ConnectionAbortedError):
                return
        if u.path == "/_log":
            return self._send(200, "application/json", json.dumps(LOG))
        self._send(404, "text/plain", "nope")

    def do_POST(self):
        u = urlparse(self.path)
        q = {k: v[0] for k, v in parse_qs(u.query).items()}
        if u.path == "/_reset":
            LOG.clear()
            STATE.clear()
            STATE.update(copy.deepcopy(INITIAL))
            if q.get("state"):
                STATE.update(json.loads(q["state"]))
            return self._send(200, "application/json", '{"ok":true}')
        LOG.append({"path": u.path, "q": q})
        if u.path == "/gameday/set":
            apply_set(q)
            return self._send(200, "application/json", '{"ok":true}')
        if u.path == "/gameday/action":
            return self._send(200, "application/json", '{"ok":true}')
        if u.path.split("/")[1] in ("switch", "number", "select", "button", "update"):
            return self._send(200, "text/plain", "")
        self._send(404, "text/plain", "nope")


if __name__ == "__main__":
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8765
    ThreadingHTTPServer(("127.0.0.1", port), H).serve_forever()
