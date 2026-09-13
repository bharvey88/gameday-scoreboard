# The panel tells you what's wrong, v1.4.0

Date: 2026-09-13. Status: draft for Brandon's review.

## Why

Today the panel keeps drawing the last score, ticker scrolling, when it has
lost Wi-Fi or cannot reach ESPN. It looks alive and is not. A customer whose
router changed cannot even set it up again, because Bluetooth was switched off
when Wi-Fi first came up and nothing turns it back on. The only fix is a power
cycle nobody told them about. Separately, a college team in a favorite slot
never shows a game, and the boot screen prints an address that does not work
when typed, next to a button that erases Wi-Fi with no warning.

Seven fixes, one release. No security changes: the panel trusts the home
network, and that stays as it is (say so in the README).

## 1. Wi-Fi lost

`firmware/gameday-common.yaml`, `firmware/pages/boot.yaml`.

- `wifi: on_disconnect:` turns Bluetooth back on (`ble.enable`) and brings the
  boot overlay back with the lines `Wi-Fi lost` / `open the` / `Game Day app`
  (single panel: `Wi-Fi` / `lost` / `open app`). Same resume-and-unhide steps
  the `esp32_improv.on_start` handler already does, so `boot_check` polls again.
- `esp32_improv` advertises on its own while Wi-Fi is down, so the app's setup
  and "waiting for setup" screens find the panel again with no app change.
- `wifi: on_connect:` keeps `ble.disable`, and `boot_check`'s poll repaints the
  normal connected lines, so a router reboot resolves itself with no reboot.
- Self-reboot: a `60s` interval counts minutes with no STA connection in a
  global; at 10 it calls `App.safe_reboot()`. Reset to 0 on connect. Not armed
  while the panel has no saved network (fresh unit in setup mode).
- Memory: Bluetooth coming back is what caused the 1.3.4 reboot loop. Add a
  `Free Heap (internal)` diagnostic sensor next to the PSRAM one, enabled by
  default, and log it on `on_disconnect` and `on_connect`. Target, checked on the
  bench: after Wi-Fi loss and recovery, three ESPN fetches succeed and the
  internal heap sits above 40 KB.

## 2. Stale scores in words

`components/gameday/gameday.cpp`, `gameday.h`, `espn_parse.cpp` (status_text).

- New member `last_good_ms_`, set whenever a game poll succeeds. New state
  document key `stale_s` (integer seconds since the last good poll, 0 when
  fresh) at top level next to `misses`. `misses` stays for the page.
- After three misses, the ticker gets `| no update for N min` (N from
  `stale_s`, rounded down, `1 min` minimum) instead of the bare ` *`.
- If the panel has no network at all, `emit_()` is still skipped, so the
  Wi-Fi lost overlay from part 1 is what the customer sees.
- The device page (`app.js`) shows the same words under the status line when
  `stale_s` is present. The app's stale badge switches from `game.m` to
  `stale_s` in its own release; until then it just never shows, as today.

## 3. College favorites

`components/gameday/gameday.cpp` line 916, `tests/`.

- In `run_job_()`, after `j.schedule = s;`, set
  `j.schedule.league = (uint8_t) j.team->league;`. `fetch_game_()` reads that
  field in favorites mode and today gets 0 (NFL) for every freshly fetched
  schedule, so a college favorite asks the NFL scoreboard and never finds its
  game.
- Host test: `tests/Makefile` compiles only the parser, so add a small
  `tests/test_favorites.cpp` that exercises the schedule-then-game step with a
  stub fetch and asserts the league survives. Bench check: Alabama in slot 1
  shows a next-game card within one poll.

## 4. Boot screen and button hold

`firmware/pages/boot.yaml`.

- Normal connect lines print `hostname + ".local"` like the button path does.
  Wide: `GAME DAY APP` / `or visit` / `gameday-2f6a70.local` / `10.0.0.5`.
  Single: `GAME DAY` / `APP` / `gameday-2f6a70.local` / `10.0.0.5`.
- The hold is timed on press, not release. A `100ms` interval runs while the
  button is down:

  | Held | Panel |
  |---|---|
  | under 1.5 s | unchanged |
  | 1.5 s | full address overlay, stays while held |
  | 5 s | `Keep holding` / `to reset` / `Wi-Fi`, bar fills from 5 s to 10 s |
  | 10 s | `Wi-Fi reset` / `restarting`, then `reset_wifi.press()` |

  Release before 10 s: the address stays 20 s as today, nothing is reset. The
  bar reuses `boot_bar` with `boot_anim` suspended during the hold.

## 5. Switch-every minimum of 1 minute

The live modes' "Switch every" setting cannot go below 2 minutes. Brandon
wants 1. Firmware: the clamp in `set_rotate_minutes()`
(`components/gameday/gameday.cpp` line 288) becomes `< 1`. Device page: the
`rotate` and `favrotate` inputs' `min` and the JavaScript clamp
(`firmware/web/app.js` lines 85, 97, 337 and the favorites equivalent) become
1, then rebuild `bundle.js`. App: both steppers in `ModeBarView.swift`
(lines 56 and 75) become `1...30`, in the app session. Saved values are
unaffected.

## 6. Never sit on an old game

Field report 2026-09-13: after a night with the display off (Power switch,
the chip kept running) the panel showed last week's game with `misses` at 0,
so ESPN was answering and the firmware was polling the wrong event.
`components/gameday/gameday.cpp`.

- In `apply_job_()`, when the polled game is POST and its kickoff is more
  than 6 hours old, or ESPN reports the event as not found on a good
  document, set `schedule_fetched_ms_ = 0` and reschedule immediately so the
  team endpoint is re-read now instead of at the 6 hour mark.
- Call `emit_()` on every successful poll, not only on a change, so the
  display can never lag the state document.
- Deadline on the worker: if `busy_` has been set for more than 60 s with no
  `job_done_`, log it, clear `busy_`, and start a fresh job. Today a worker
  that dies silently blocks the loop forever and Refresh Now cannot clear it.
- Bench: (h) set the team to one that played yesterday, wait past the final
  linger, confirm the next game card appears within one poll.

## 7. Live modes fall back to your team

Field report 2026-09-13: the panel sat in "Live college" on an NFL Sunday
showing nothing useful, because the mode means "a live college game or
nothing". `components/gameday/gameday.cpp` (start_job_ ~825, apply_job_
~926 live branch ~938-984, emit_ ~1027, rebuild_state_ ~1221).

- When a live scan returns zero games, the panel shows the My team card
  for the saved team: live if that team is playing, otherwise its next-game
  card with kickoff and odds, exactly as My team mode draws it. The ticker
  gets a leading `No live college games, showing DAL |` (league named from
  the mode). Mode stays what the customer chose; the state document adds
  `fallback: true` so the page and app can show the same note on the mode
  row.
- The scan keeps running every 2 minutes (`NO_LIVE_RESCAN`). The first scan
  that finds a game switches back to it; when that game ends the existing
  rotate and rescan logic applies, and an empty scan drops back to the team
  card.
- In the fallback the team card polls on the My team schedule (schedule
  re-read, POST linger, intervals by phase) so it is never a stale card.
- No team saved (fresh unit): keep today's text, "No live college games
  right now".
- Bench: (i) Live college on a Sunday shows the DAL next-game card with the
  note; (j) Live NFL on a Saturday the same; (k) switch to Live college on a
  Saturday morning before kickoff, see the DAL card, see it replaced by the
  first live college game.

## Not in this release

App changes (wrong-password error, 2.4 GHz note, connect timeout, stale badge,
offline card) are the next session. No polling or ESPN traffic changes.

## Testing

- Host: existing 186 parser checks plus the favorites test, in CI.
- Bench, Brandon: (a) change the router's Wi-Fi password, panel shows Wi-Fi
  lost within 60 s, app setup finds it over Bluetooth, joins, normal screen
  returns; (b) reboot the router only, panel recovers on its own with no
  reboot; (c) block ESPN at the router during a game, ticker says
  `no update for N min` within 3 polls, clears when unblocked; (d) leave Wi-Fi
  off 11 min, panel reboots; (e) Alabama in slot 1 shows a game; (f) hold the
  button for 2 s, 6 s, and 11 s and see each stage; (g) internal heap logged
  through (a) and (b) stays above 40 KB.
