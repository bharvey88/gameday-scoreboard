#!/usr/bin/env python3
"""Watch panels through a burn-in and report which ones failed.

Every --interval seconds it reads each panel's event stream (the one the
device page uses) for uptime, free heap and Wi-Fi signal, and /gameday/state
for ESPN fetch misses and how old the score is. Every request is a GET. Each
reading is a row in burnin/<start time>.csv. When --hours runs out, or on
Ctrl+C, it prints one line per panel: PASS, or FAIL and why.

    python scripts/burnin_monitor.py gameday-2f6a70.local gameday-1a2b3c.local
    python scripts/burnin_monitor.py --hours 48 gameday-2f6a70.local
    python scripts/burnin_monitor.py --selftest

A panel fails if it restarted, missed more than 2% of readings, or its free
internal heap went below --min-heap KiB (40 by default).
"""

import argparse
import csv
import datetime
import http.client
import json
import sys
import time
from pathlib import Path

FIELDS = ["time", "host", "ok", "uptime_s", "rebooted", "heap_internal_kib", "heap_psram_kib",
          "rssi_dbm", "version", "misses", "stale_s", "error"]
SENSORS = {
    "sensor/Free Heap (internal)": "heap_internal_kib",
    "sensor/Free Heap (PSRAM)": "heap_psram_kib",
    "sensor/RSSI": "rssi_dbm",
}
# Readings more than this far short of the expected uptime mean a restart
REBOOT_SLACK_S = 30
MAX_MISSED = 0.02


def parse_events(lines, wanted):
    """Fold Server-Sent Events lines into a reading, stopping once complete.

    The stream opens with a ping event carrying the uptime and version, then
    one state event per entity.
    """
    reading = {}
    event, data = "", ""
    for raw in lines:
        line = raw.rstrip("\r\n")
        if line.startswith("event:"):
            event = line[6:].strip()
        elif line.startswith("data:"):
            data += line[5:].strip()
        elif line == "":
            if data:
                try:
                    payload = json.loads(data)
                except ValueError:
                    payload = None
                if isinstance(payload, dict):
                    if event == "ping" and "uptime" in payload:
                        reading["uptime_s"] = int(payload["uptime"])
                        if "comment" in payload:
                            reading["version"] = payload["comment"]
                    elif event == "state" and payload.get("id") in SENSORS:
                        reading[SENSORS[payload["id"]]] = payload.get("value")
            event, data = "", ""
            if wanted <= reading.keys():
                break
    return reading


def stream_lines(host, timeout):
    conn = http.client.HTTPConnection(host, 80, timeout=timeout)
    try:
        conn.request("GET", "/events", headers={"Accept": "text/event-stream"})
        resp = conn.getresponse()
        if resp.status != 200:
            raise OSError(f"/events answered {resp.status}")
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            line = resp.readline()
            if not line:
                return
            yield line.decode("utf-8", "replace")
    finally:
        conn.close()


def read_panel(host, timeout=6):
    wanted = {"uptime_s", *SENSORS.values()}
    lines = stream_lines(host, timeout)
    try:
        reading = parse_events(lines, wanted)
    finally:
        lines.close()
    if "uptime_s" not in reading:
        raise OSError("no uptime in the event stream")
    conn = http.client.HTTPConnection(host, 80, timeout=timeout)
    try:
        conn.request("GET", "/gameday/state")
        resp = conn.getresponse()
        if resp.status == 200:
            state = json.loads(resp.read())
            reading["misses"] = state.get("misses")
            reading["stale_s"] = state.get("stale_s")
            reading.setdefault("version", state.get("version"))
    finally:
        conn.close()
    return reading


def restarted(prev, now_uptime, now_t):
    """True when the uptime is well short of where the last reading put it."""
    if prev is None:
        return False
    prev_t, prev_uptime = prev
    return now_uptime + REBOOT_SLACK_S < prev_uptime + (now_t - prev_t)


def summarize(rows, min_heap):
    """One verdict per host from its rows: (host, passed, reasons, facts)."""
    out = []
    for host in dict.fromkeys(r["host"] for r in rows):
        mine = [r for r in rows if r["host"] == host]
        good = [r for r in mine if r["ok"]]
        reboots = sum(1 for r in good if r["rebooted"])
        missed = len(mine) - len(good)
        heaps = [r["heap_internal_kib"] for r in good if r.get("heap_internal_kib") is not None]
        rssi = [r["rssi_dbm"] for r in good if r.get("rssi_dbm") is not None]
        stale = [r["stale_s"] for r in good if r.get("stale_s") is not None]
        reasons = []
        if reboots:
            reasons.append(f"restarted {reboots}x")
        if mine and missed / len(mine) > MAX_MISSED:
            reasons.append(f"missed {missed} of {len(mine)} readings")
        if heaps and min(heaps) < min_heap:
            reasons.append(f"internal heap fell to {min(heaps):.0f} KiB")
        if not good:
            reasons.append("never answered")
        facts = f"{len(good)}/{len(mine)} readings"
        if heaps:
            facts += f", internal heap min {min(heaps):.0f} KiB"
        if rssi:
            facts += f", RSSI {min(rssi):.0f} to {max(rssi):.0f} dBm"
        if stale:
            facts += f", score age max {max(stale):.0f} s"
        out.append((host, not reasons, reasons, facts))
    return out


def selftest():
    stream = [
        "retry: 30000\n", "id: 1758675\n", "event: ping\n",
        'data: {"title":"Game Day Scoreboard 2f6a70","comment":"v1.5.4","ota":true,"log":true,"lang":"en","uptime":1758}\n',
        "\n", "event: state\n",
        'data: {"id":"sensor/Effect FPS","domain":"sensor","value":null,"state":"NA"}\n', "\n",
        "event: state\n",
        'data: {"id":"sensor/Free Heap (PSRAM)","domain":"sensor","value":6131.73,"state":"6131.7 KiB"}\n', "\n",
        "event: state\n",
        'data: {"id":"sensor/Free Heap (internal)","domain":"sensor","value":78.51953,"state":"78.5 KiB"}\n', "\n",
        "event: state\n", 'data: {"id":"sensor/RSSI","domain":"sensor","value":-30,"state":"-30 dBm"}\n', "\n",
        "event: state\n", 'data: {"id":"sensor/never read","value":1}\n', "\n",
    ]
    got = parse_events(iter(stream), {"uptime_s", *SENSORS.values()})
    assert got == {"uptime_s": 1758, "version": "v1.5.4", "heap_psram_kib": 6131.73,
                   "heap_internal_kib": 78.51953, "rssi_dbm": -30}, got
    assert parse_events(iter(["event: ping\n", "data: not json\n", "\n"]), {"uptime_s"}) == {}

    assert not restarted(None, 5, 100.0)
    assert not restarted((0.0, 1000), 1060, 60.0)
    assert not restarted((0.0, 1000), 1045, 60.0)  # slow reading, within the slack
    assert restarted((0.0, 1000), 20, 60.0)
    assert restarted((0.0, 10), 40, 120.0)  # restarted and already up longer than before

    def row(host, ok=True, rebooted=False, heap=78.0):
        return {"host": host, "ok": ok, "rebooted": rebooted, "heap_internal_kib": heap,
                "rssi_dbm": -50, "stale_s": 30}

    rows = [row("a") for _ in range(100)]
    rows += [row("b") for _ in range(97)] + [row("b", ok=False) for _ in range(3)]
    rows += [row("c") for _ in range(99)] + [row("c", rebooted=True)]
    rows += [row("d", heap=35.0)] + [row("d") for _ in range(99)]
    rows += [row("e", ok=False)]
    verdict = {h: (passed, reasons) for h, passed, reasons, _ in summarize(rows, 40)}
    assert verdict["a"] == (True, [])
    assert verdict["b"] == (False, ["missed 3 of 100 readings"])
    assert verdict["c"] == (False, ["restarted 1x"])
    assert verdict["d"] == (False, ["internal heap fell to 35 KiB"])
    assert verdict["e"][0] is False and "never answered" in verdict["e"][1]
    print("selftest ok")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("hosts", nargs="*", help="panel addresses, e.g. gameday-2f6a70.local")
    ap.add_argument("--hours", type=float, default=48)
    ap.add_argument("--interval", type=int, default=60, help="seconds between readings (default 60)")
    ap.add_argument("--min-heap", type=float, default=40, help="fail below this free internal heap, KiB")
    ap.add_argument("--out", type=Path, default=Path("burnin"))
    ap.add_argument("--selftest", action="store_true")
    args = ap.parse_args()
    if args.selftest:
        selftest()
        return 0
    if not args.hosts:
        ap.error("name at least one panel")

    args.out.mkdir(parents=True, exist_ok=True)
    started = datetime.datetime.now()
    path = args.out / f"{started:%Y%m%d-%H%M}.csv"
    print(f"Logging to {path}. Ctrl+C stops early and prints the verdict.")
    rows, last = [], {}
    end = time.monotonic() + args.hours * 3600
    try:
        with path.open("w", newline="", encoding="utf-8") as f:
            w = csv.DictWriter(f, fieldnames=FIELDS)
            w.writeheader()
            while time.monotonic() < end:
                tick = time.monotonic()
                for host in args.hosts:
                    row = dict.fromkeys(FIELDS, None)
                    row.update(time=datetime.datetime.now().isoformat(timespec="seconds"), host=host,
                               ok=False, rebooted=False)
                    try:
                        reading = read_panel(host)
                        now = time.monotonic()
                        row.update(reading, ok=True)
                        row["rebooted"] = restarted(last.get(host), reading["uptime_s"], now)
                        last[host] = (now, reading["uptime_s"])
                        note = "  RESTARTED" if row["rebooted"] else ""
                        heap = row["heap_internal_kib"]
                        heap = "?" if heap is None else f"{heap:.1f}"
                        print(f"{row['time']} {host} up {reading['uptime_s']} s, "
                              f"heap {heap} KiB, RSSI {row['rssi_dbm']}{note}")
                    except (OSError, http.client.HTTPException, ValueError) as e:
                        row["error"] = str(e)[:120]
                        print(f"{row['time']} {host} no answer: {row['error']}")
                    rows.append(row)
                    w.writerow(row)
                    f.flush()
                time.sleep(max(0.0, args.interval - (time.monotonic() - tick)))
    except KeyboardInterrupt:
        pass

    elapsed = datetime.datetime.now() - started
    print(f"\nRan {elapsed.total_seconds() / 3600:.1f} h. Log: {path}")
    failed = 0
    for host, passed, reasons, facts in summarize(rows, args.min_heap):
        failed += not passed
        verdict = "PASS" if passed else "FAIL: " + "; ".join(reasons)
        print(f"{host}  {verdict}  ({facts})")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
