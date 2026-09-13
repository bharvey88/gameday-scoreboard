# Panel Tells You What's Wrong (v1.4.0) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make the panel say what is wrong instead of silently drawing a stale board, and fix the four product bugs found in the field.

**Architecture:** Seven independent changes across three layers: the ESPHome YAML boot page (overlay text, Wi-Fi handlers, button timing), the `gameday` C++ component (staleness tracking, league propagation, schedule recovery, live fallback), and the device web page (one input range). Each task is independently committable and leaves the firmware compiling. Only the pure-parser layer is host-testable, so tasks touching `gameday.cpp` internals are verified by compile plus Brandon's bench list.

**Tech Stack:** ESPHome 2026.8.2 (venv at `~/development/tools/esphome-venv`), LVGL, C++17, ArduinoJson 7.4.3, vanilla JS bundled by `scripts/build_web.py`.

---

## Before you start

- Work in this worktree: `~/development/gameday-scoreboard-tells`, branch `panel-tells-whats-wrong`.
- Commit as `bharvey88 <8107750+bharvey88@users.noreply.github.com>`, no Claude credit, multi-line messages via `git commit -F`.
- Host tests: `make -C tests`. Baseline before any change is **186 checks, 0 failures**.
- Config check (no flash): `~/development/tools/esphome-venv/bin/esphome config firmware/gameday.yaml`
- Rebuild the page bundle after touching `firmware/web/app.js`: `python scripts/build_web.py`
- Read-only on the live panel unless Brandon asks. GET only, no POSTs.
- Nothing is verified until Brandon runs the bench. Do not claim a part works.

## Deviations from the spec, and why

Three places where the spec does not match the code. Each was checked against the
source before writing this plan.

**1. Part 3's host test cannot be written as specified.** The spec asks for
`tests/test_favorites.cpp` exercising "the schedule-then-game step with a stub
fetch". `tests/Makefile` compiles only `espn_parse.cpp`; `run_job_()` lives in
`gameday.cpp`, which needs ESPHome headers and cannot link on the host. Instead
the league stamp is extracted into a pure inline in `espn_parse.h`
(`adopt_league`), host-tested there, and called from both places in
`gameday.cpp`. This also removes a duplicated assignment. Task 1.

**2. Part 6's "call `emit_()` on every successful poll, not only on a change" is
already true** on the main path. `apply_job_()` calls `this->emit_(splash)`
unconditionally after a good poll (`gameday.cpp:1023`), and `emit_()` has no
change guard. The only place a good poll does not emit is `apply_fav_job_()`,
where a poll for a favorite that is **not** the shown team calls
`rebuild_state_()` instead, which is correct: an off-screen team should not
repaint the board. So this bullet is dropped as a no-op.

**Consequence for the open question.** The handoff's theory was that this bullet
explained "an old game with outdated details while the state document said
NOT_FOUND". It cannot, because the emit is already unconditional. The remaining
candidates are the event-staleness bug (Task 6, real and fixed here) or a
genuine display bug in the LVGL layer that nobody has seen in code. Brandon's
photo is still the thing that would settle it. Do not close the open question
on the back of this release.

**3. Part 2's device-page work is unnecessary.** `rebuild_state_()` already
publishes `doc["status"] = f->status_text` (`gameday.cpp:1286`) and `app.js:435`
renders it straight into `#ticker`. Once the firmware puts "no update for N min"
into `status_text`, the page shows it with no JavaScript change. `stale_s` is
still added to the state document for the app to read later. Task 5.

## File Structure

| File | Responsibility | Tasks |
|---|---|---|
| `components/gameday/espn_parse.h` | Pure helpers, no ESPHome. Gains `adopt_league`. | 1 |
| `tests/test_parse.cpp` | Host tests. Gains `test_schedule_league`. | 1 |
| `components/gameday/gameday.h` | Component state. Gains `last_good_ms_`, `live_fallback_`, `busy_since_ms_`. | 5, 6, 9 |
| `components/gameday/gameday.cpp` | Poll loop, job worker, state document. | 1, 2, 5, 6, 9 |
| `firmware/web/app.js` | Device page. Rotate minimums only. | 2 |
| `firmware/pages/boot.yaml` | Boot/setup overlay and the boot button. | 3, 4, 7 |
| `firmware/gameday-common.yaml` | Wi-Fi handlers, diagnostics, version. | 7, 8, 10 |
| `CHANGELOG.md` | Customer-facing release notes. | 10 |

---

## Task 1: College favorites ask the right league (spec part 3)

A `Schedule` parsed from the team endpoint carries `league = 0` (NFL), because the
endpoint does not say. `fetch_game_()` picks the scoreboard endpoint from
`schedule.league` whenever `fav_index >= 0` (`gameday.cpp:787`), so on every
schedule refresh a college favorite polls the NFL scoreboard with an NCAA event
id and finds nothing. `apply_fav_job_()` already stamps the league at
`gameday.cpp:484`, but that runs *after* the failed fetch in the same job.

**Files:**
- Modify: `components/gameday/espn_parse.h` (add `adopt_league` near the other free functions)
- Modify: `tests/test_parse.cpp` (new test + call in `main()`)
- Modify: `components/gameday/gameday.cpp:916` and `:484`

- [ ] **Step 1: Write the failing test**

Add to `tests/test_parse.cpp`, immediately before `int main() {`:

```cpp
// A schedule parsed from the team endpoint does not know its own league, so
// the caller has to stamp it. Without the stamp a college favorite polls the
// NFL scoreboard and never finds its game.
static void test_schedule_league() {
  std::string json = slurp("fixtures/team_ncaa_bc.json");
  Schedule s;
  CHECK(parse_team_str(json, s));
  CHECK(s.valid);
  CHECK_EQ((int) s.league, 0);  // documents the gap: 0 is League::NFL

  adopt_league(s, League::NCAA);
  CHECK_EQ((int) s.league, (int) League::NCAA);

  std::string url = scoreboard_url((League) s.league, s.group, s.kickoff_epoch);
  CHECK(url.find("/college-football/") != std::string::npos);
  CHECK(url.find("/nfl/") == std::string::npos);
}
```

Add the call inside `main()`, after `test_favorites();`:

```cpp
  test_schedule_league();
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `make -C tests`
Expected: a **compile error**, `error: use of undeclared identifier 'adopt_league'`.

- [ ] **Step 3: Add the helper**

In `components/gameday/espn_parse.h`, add immediately after the
`std::string clock_text(const GameSnapshot &s);` declaration and before
`}  // namespace espn`:

```cpp
// The team endpoint does not say which league answered, so a freshly parsed
// Schedule carries league 0 (NFL). Favorites and the live modes choose the
// follow-up scoreboard endpoint from Schedule::league, so it must be stamped
// from the team the schedule was fetched for.
inline void adopt_league(Schedule &s, League league) { s.league = (uint8_t) league; }
```

- [ ] **Step 4: Run the test to verify it passes**

Run: `make -C tests`
Expected: `191 checks, 0 failures` (186 baseline + 5 new).

- [ ] **Step 5: Stamp the league where the schedule is fetched**

In `components/gameday/gameday.cpp`, in `run_job_()`, change:

```cpp
    j.schedule = s;
```

to:

```cpp
    j.schedule = s;
    ::espn::adopt_league(j.schedule, j.team->league);
```

- [ ] **Step 6: Remove the now-duplicated stamp**

In `apply_fav_job_()`, change:

```cpp
    e.sched = j.schedule;
    e.sched.league = (uint8_t) e.team->league;
```

to:

```cpp
    e.sched = j.schedule;
    ::espn::adopt_league(e.sched, e.team->league);
```

- [ ] **Step 7: Verify the firmware still configures**

Run: `~/development/tools/esphome-venv/bin/esphome config firmware/gameday.yaml >/dev/null && echo OK`
Expected: `OK`

- [ ] **Step 8: Commit**

```bash
git add components/gameday/espn_parse.h components/gameday/gameday.cpp tests/test_parse.cpp
git commit -F- <<'MSG'
College favorites poll the right league

A Schedule parsed from the team endpoint carries league 0, and
fetch_game_ picks the scoreboard endpoint from that field in favorites
mode, so a college favorite asked the NFL scoreboard on every schedule
refresh and found nothing.

Stamp the league in run_job_ right after the parse, through a new pure
adopt_league helper that the host test can reach. apply_fav_job_ was
already doing the same assignment by hand, too late to help the fetch in
that same job; it now calls the helper too.
MSG
```

---

## Task 2: Switch every 1 minute (spec part 5)

**Files:**
- Modify: `components/gameday/gameday.cpp:288`
- Modify: `firmware/web/app.js:85`, `:97`, `:337`, `:362`
- Regenerate: `firmware/web/bundle.js`

- [ ] **Step 1: Lower the firmware clamp**

In `set_rotate_minutes()`, change:

```cpp
  if (minutes < 2)
    minutes = 2;
```

to:

```cpp
  if (minutes < 1)
    minutes = 1;
```

- [ ] **Step 2: Lower the page inputs**

In `firmware/web/app.js` line 85, change `id="rotate" min="2"` to `id="rotate" min="1"`.

In line 97, change `id="favrotate" min="2"` to `id="favrotate" min="1"`.

- [ ] **Step 3: Lower the two JavaScript clamps**

In `rotateInput.onchange` (line ~337), change:

```js
    const v = Math.min(30, Math.max(2, Number(rotateInput.value) || 5));
```

to:

```js
    const v = Math.min(30, Math.max(1, Number(rotateInput.value) || 5));
```

In `favRotate.onchange` (line ~362), change:

```js
    const v = Math.min(30, Math.max(2, Number(favRotate.value) || 5));
```

to:

```js
    const v = Math.min(30, Math.max(1, Number(favRotate.value) || 5));
```

- [ ] **Step 4: Rebuild the bundle**

Run: `python scripts/build_web.py`
Expected: `firmware/web/bundle.js` is rewritten. Confirm with
`git diff --stat firmware/web/bundle.js` showing it changed.

- [ ] **Step 5: Verify no stale minimum survives**

Run: `grep -n 'Math.max(2,' firmware/web/app.js firmware/web/bundle.js; grep -n 'min="2"' firmware/web/app.js firmware/web/bundle.js`
Expected: no output for the rotate inputs. (`lockon` keeps `min="5"`; it is a
different setting and must not change.)

- [ ] **Step 6: Verify the firmware still configures**

Run: `~/development/tools/esphome-venv/bin/esphome config firmware/gameday.yaml >/dev/null && echo OK`
Expected: `OK`

- [ ] **Step 7: Commit**

```bash
git add components/gameday/gameday.cpp firmware/web/app.js firmware/web/bundle.js
git commit -F- <<'MSG'
Switch every can go down to 1 minute

The firmware clamp, the two page inputs and the two JavaScript clamps
all said 2. Saved values are unaffected; the app steppers move in the
app's own release.
MSG
```

---

## Task 3: Boot screen shows a typable address (spec part 4, first half)

The connected lines print `hostname()`; the button-hold path prints
`hostname() + ".local"`. Typing the former into a browser does not work.

**Files:**
- Modify: `firmware/pages/boot.yaml:120-123`

- [ ] **Step 1: Add `.local` to the normal connect lines**

In the `boot_refresh` script, in the `else` branch of `if (via_button)`, change:

```cpp
              if (wide)
                id(boot_set_lines).execute("GAME DAY APP", "or visit", id(gameday_ctl).hostname(), "", false);
              else
                id(boot_set_lines).execute("GAME DAY", "APP", id(gameday_ctl).hostname(), "", false);
```

to:

```cpp
              if (wide)
                id(boot_set_lines).execute("GAME DAY APP", "or visit", id(gameday_ctl).hostname() + ".local", "", false);
              else
                id(boot_set_lines).execute("GAME DAY", "APP", id(gameday_ctl).hostname() + ".local", "", false);
```

- [ ] **Step 2: Verify it configures**

Run: `~/development/tools/esphome-venv/bin/esphome config firmware/gameday.yaml >/dev/null && echo OK`
Expected: `OK`

- [ ] **Step 3: Commit**

```bash
git add firmware/pages/boot.yaml
git commit -m "Boot screen prints an address you can actually type"
```

---

## Task 4: Button hold warns before it erases Wi-Fi (spec part 4, second half)

Today the hold is measured on release, so the panel shows nothing while you hold
it and a 10 second hold silently wipes the saved network.

**Files:**
- Modify: `firmware/pages/boot.yaml` (globals, a new script, a new interval, the binary_sensor block)

- [ ] **Step 1: Add a global for the hold stage**

In the `globals:` block, after `boot_button_press_ms`, add:

```yaml
  - id: boot_hold_stage
    type: int
    initial_value: '0'
```

- [ ] **Step 2: Add the hold-progress script**

In the `script:` block, after `boot_show_address`, add:

```yaml
  # Boot button held down: stage 1 shows the address, stage 2 warns that a
  # reset is coming with a bar that fills from 5s to 10s, stage 3 resets.
  # Timed from the press so the customer sees the warning before anything
  # is erased.
  - id: boot_hold_tick
    then:
      - lambda: |-
          if (id(boot_button_press_ms) == 0) return;
          const uint32_t held = millis() - id(boot_button_press_ms);
          const bool wide = lv_disp_get_hor_res(lv_disp_get_default()) >= 128;
          if (held >= 10000) {
            if (id(boot_hold_stage) == 3) return;
            id(boot_hold_stage) = 3;
            if (wide)
              id(boot_set_lines).execute("Wi-Fi reset", "restarting", "", "", false);
            else
              id(boot_set_lines).execute("Wi-Fi reset", "restarting", "", "", false);
            id(boot_button_press_ms) = 0;
            id(reset_wifi).press();
            return;
          }
          if (held >= 5000) {
            if (id(boot_hold_stage) != 2) {
              id(boot_hold_stage) = 2;
              id(boot_anim).suspend();
              if (wide)
                id(boot_set_lines).execute("Keep holding", "to reset", "Wi-Fi", "", true);
              else
                id(boot_set_lines).execute("Keep holding", "to reset", "Wi-Fi", "", true);
            }
            int pct = (int) ((held - 5000) / 50);  // 5000ms span -> 0..100
            if (pct > 100) pct = 100;
            lv_bar_set_value(id(boot_bar), pct, LV_ANIM_OFF);
            return;
          }
          if (held >= 1500 && id(boot_hold_stage) < 1) {
            id(boot_hold_stage) = 1;
            id(boot_show_address).execute();
          }
```

- [ ] **Step 3: Add the interval that drives it**

In the `interval:` block, after the `boot_anim` entry, add:

```yaml
  - interval: 100ms
    id: boot_hold_check
    then:
      - script.execute: boot_hold_tick
```

- [ ] **Step 4: Rewrite the button handlers to time from the press**

Replace the whole `binary_sensor:` block with:

```yaml
# Boot button: the hold is timed from the press so the panel can warn before
# it erases anything. 1.5s shows the address, 5s starts the "keep holding"
# warning with a filling bar, 10s resets Wi-Fi. Releasing before 10s leaves
# the address up for 20s as before and resets nothing.
binary_sensor:
  - id: !extend power_button
    on_press:
      - lambda: |-
          id(boot_button_press_ms) = millis() == 0 ? 1 : millis();
          id(boot_hold_stage) = 0;
    on_release:
      - lambda: |-
          const bool warned = id(boot_hold_stage) == 2;
          id(boot_button_press_ms) = 0;
          id(boot_hold_stage) = 0;
          if (warned) {
            // Back out of the warning: restore the address screen and its bar.
            id(boot_anim).resume();
            id(boot_show_address).execute();
          }
```

- [ ] **Step 5: Verify it configures**

Run: `~/development/tools/esphome-venv/bin/esphome config firmware/gameday.yaml >/dev/null && echo OK`
Expected: `OK`

- [ ] **Step 6: Commit**

```bash
git add firmware/pages/boot.yaml
git commit -F- <<'MSG'
Button hold warns before it erases Wi-Fi

The hold was measured on release, so the panel showed nothing while the
button was down and a ten second hold wiped the saved network with no
warning at all. Time it from the press instead: the address at 1.5s, a
"keep holding to reset Wi-Fi" warning with a filling bar from 5s, and
the reset at 10s. Letting go before 10s resets nothing.
MSG
```

---

## Task 5: Stale scores in words (spec part 2)

After three misses the ticker appends a bare ` *`. Replace it with words, and
publish `stale_s` so the app can read it in its own release.

**Files:**
- Modify: `components/gameday/gameday.h` (new member near `misses_`)
- Modify: `components/gameday/gameday.cpp` (mark good polls, ticker text, state key)

- [ ] **Step 1: Add the member**

In `components/gameday/gameday.h`, next to `uint8_t misses_{0};`, add:

```cpp
  uint32_t last_good_ms_{0};  // millis() of the last successful game poll, 0 = never
```

- [ ] **Step 2: Add a helper that records a good poll**

In `components/gameday/gameday.h`, in the same private section, add:

```cpp
  // A game poll came back clean: clear the miss count and start the clock over.
  void mark_good_poll_() {
    this->misses_ = 0;
    this->last_good_ms_ = millis() == 0 ? 1 : millis();
  }
  // Seconds since the last good poll, 0 when fresh or when nothing has
  // succeeded yet (the Wi-Fi lost overlay covers that case).
  uint32_t stale_seconds_() const {
    if (this->last_good_ms_ == 0)
      return 0;
    return (millis() - this->last_good_ms_) / 1000;
  }
```

- [ ] **Step 3: Record good polls at the four sites that mean "a game poll succeeded"**

In `components/gameday/gameday.cpp`, replace `this->misses_ = 0;` with
`this->mark_good_poll_();` at these four places only:

- in `apply_fav_job_()`, the line after `e.final_epoch = tnow;` handling (line ~509)
- in `apply_job_()` scan branch, inside `if (pick.empty())` (line ~957)
- in `apply_job_()` schedule branch, inside `if (j.no_event)` (line ~1000)
- in `apply_job_()` after `this->game_ = j.game;` (line ~1014)

Leave the other `misses_ = 0;` sites alone: they are team changes and resets,
not poll results.

- [ ] **Step 4: Replace the asterisk with words**

In `emit_()`, change:

```cpp
  if (this->misses_ >= 3)
    f.status_text += " *";
```

to:

```cpp
  if (this->misses_ >= 3) {
    uint32_t mins = this->stale_seconds_() / 60;
    if (mins < 1)
      mins = 1;
    f.status_text += " | no update for " + std::to_string(mins) + " min";
  }
```

- [ ] **Step 5: Publish `stale_s` in the state document**

In `rebuild_state_()`, after `doc["misses"] = this->misses_;`, add:

```cpp
  doc["stale_s"] = this->stale_seconds_();
```

- [ ] **Step 6: Verify it configures and the host tests still pass**

Run: `make -C tests && ~/development/tools/esphome-venv/bin/esphome config firmware/gameday.yaml >/dev/null && echo OK`
Expected: `191 checks, 0 failures` then `OK`.

- [ ] **Step 7: Commit**

```bash
git add components/gameday/gameday.h components/gameday/gameday.cpp
git commit -F- <<'MSG'
Say how long the scores have been stale

After three missed polls the ticker appended a bare asterisk that meant
nothing to anyone. Track the time of the last good poll and say "no
update for N min" instead, and publish stale_s in the state document so
the app can show the same thing in its own release.

The device page needs no change: it already renders status straight into
the ticker.
MSG
```

---

## Task 6: Never sit on an old game (spec part 6)

Two real problems. A POST game that is hours old keeps being polled until the
6 hour schedule interval comes round, and a worker task that dies silently
leaves `busy_` set forever, which blocks the loop and makes Refresh Now do
nothing.

Note: the spec's third bullet (emit on every poll) is dropped. See "Deviations".

**Files:**
- Modify: `components/gameday/gameday.h` (new member)
- Modify: `components/gameday/gameday.cpp` (`apply_job_`, `loop`, `start_job_`, `start_fav_job_`)

- [ ] **Step 1: Add the worker deadline member**

In `components/gameday/gameday.h`, next to `last_good_ms_`, add:

```cpp
  uint32_t busy_since_ms_{0};  // millis() when the worker task was started
```

- [ ] **Step 2: Stamp it wherever the worker is started**

In `components/gameday/gameday.cpp`, every place that reads `this->busy_ = true;`
(there are four: `start_fav_job_` ~457, and three in `start_job_` ~846, ~861,
~878) becomes:

```cpp
    this->busy_ = true;
    this->busy_since_ms_ = millis() == 0 ? 1 : millis();
```

- [ ] **Step 3: Add the deadline to the loop**

In `loop()`, change:

```cpp
  if (this->busy_) {
    if (!this->job_done_)
      return;
```

to:

```cpp
  if (this->busy_) {
    if (!this->job_done_) {
      // A worker that died without setting job_done_ would block the loop
      // forever and Refresh Now could never clear it. Give up after 60s.
      if (this->busy_since_ms_ != 0 && (millis() - this->busy_since_ms_) >= 60000) {
        ESP_LOGW(TAG, "Fetch task did not finish in 60s, starting a fresh job");
        this->busy_ = false;
        this->busy_since_ms_ = 0;
        this->generation_++;  // disown anything the old task may still write
        this->schedule_next_(0);
      }
      return;
    }
```

- [ ] **Step 4: Clear the stamp when a job completes**

In `loop()`, immediately after `this->busy_ = false;` in the completion path, add:

```cpp
    this->busy_since_ms_ = 0;
```

- [ ] **Step 5: Force a schedule re-read when the polled game is old or gone**

In `apply_job_()`, after `this->game_ = j.game;` and before the splash is
decided, add:

```cpp
  // Never sit on last week's game. If the event we are polling finished
  // hours ago, or ESPN no longer knows it on a good document, the team
  // endpoint has moved on: re-read it now instead of at the 6 hour mark.
  {
    int64_t tnow = (int64_t) this->time_->timestamp_now();
    bool long_final = this->game_.state == GameState::POST && this->game_.kickoff_epoch != 0 &&
                      tnow - this->game_.kickoff_epoch > 6 * 3600;
    bool vanished = this->game_.state == GameState::NOT_FOUND;
    if (!this->live_mode_() && (long_final || vanished)) {
      ESP_LOGI(TAG, "Polled game is %s, re-reading the schedule", long_final ? "long over" : "gone");
      this->schedule_fetched_ms_ = 0;
      this->post_since_ms_ = 0;
      this->prev_ = GameSnapshot{};
      this->game_ = GameSnapshot{};
      this->emit_({});
      this->schedule_next_(0);
      return;
    }
  }
```

**Placement:** in `apply_job_()` the tail reads `this->prev_ = this->game_;`,
`this->game_ = j.game;`, then `this->mark_good_poll_();` (was `this->misses_ = 0;`
before Task 5). Insert this block immediately after `this->mark_good_poll_();`
and before the `::espn::Splash splash =` line, so a good poll is still recorded
before the early return.

- [ ] **Step 6: Verify it configures and the host tests still pass**

Run: `make -C tests && ~/development/tools/esphome-venv/bin/esphome config firmware/gameday.yaml >/dev/null && echo OK`
Expected: `191 checks, 0 failures` then `OK`.

- [ ] **Step 7: Commit**

```bash
git add components/gameday/gameday.h components/gameday/gameday.cpp
git commit -F- <<'MSG'
Never sit on an old game

Field report: after a night with the display off the panel showed last
week's game with misses at 0, so ESPN was answering and the firmware was
polling the wrong event. A finished game was kept until the six hour
schedule interval came round.

Re-read the team endpoint straight away when the polled event finished
more than six hours ago or ESPN no longer knows it. Separately, put a 60
second deadline on the worker task: one that died without setting
job_done_ left busy_ set forever, which blocked the loop and made
Refresh Now do nothing.
MSG
```

---

## Task 7: Wi-Fi lost says so, and Bluetooth comes back (spec part 1, first half)

The panel keeps drawing the last score with the ticker scrolling when Wi-Fi is
gone, and Bluetooth was disabled on first connect and never re-enabled, so a
customer whose router password changed cannot re-run setup at all.

**This is the highest-risk task in the release.** Bluetooth coming back is what
caused the v1.3.4 reboot loop. The heap sensor exists so Brandon can watch for
its return on the bench.

**Files:**
- Modify: `firmware/gameday-common.yaml` (`wifi:` handlers, `sensor:`)
- Modify: `firmware/pages/boot.yaml` (a script for the lost overlay)

- [ ] **Step 1: Add the "Wi-Fi lost" overlay script**

In `firmware/pages/boot.yaml`, in the `script:` block after `boot_hold_tick`, add:

```yaml
  # Wi-Fi went away. Undo boot_hide, cancel any pending hide, and say what
  # happened instead of leaving a stale score on the board. Bluetooth is
  # switched back on by the wifi: on_disconnect handler so the app can find
  # the panel again, exactly as it does on a fresh unit.
  - id: boot_wifi_lost
    then:
      - component.resume: boot_check
      - component.resume: boot_anim
      - script.stop: boot_hide_later
      - lvgl.obj.update:
          id: boot_overlay
          hidden: false
      - lambda: |-
          const bool wide = lv_disp_get_hor_res(lv_disp_get_default()) >= 128;
          if (wide)
            id(boot_set_lines).execute("Wi-Fi lost", "open the", "Game Day app", "", true);
          else
            id(boot_set_lines).execute("Wi-Fi", "lost", "open app", "", true);
```

- [ ] **Step 2: Let the overlay come back after it has been hidden once**

`boot_refresh` only arms `boot_hide_later` when `boot_ip_shown_ms` is 0, so once
the panel has connected the overlay never returns on its own. In
`firmware/pages/boot.yaml`, in `boot_wifi_lost`, add as the first action:

```yaml
      - lambda: 'id(boot_ip_shown_ms) = 0;'
```

so that the next successful connect re-arms the normal hide timer.

- [ ] **Step 3: Wire the Wi-Fi handlers**

In `firmware/gameday-common.yaml`, replace:

```yaml
  on_connect:
    - ble.disable:
```

with:

```yaml
  on_connect:
    - ble.disable:
    - lambda: |-
        ESP_LOGI("wifi", "Connected. Internal heap free: %u bytes",
                 (unsigned) heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
  # Wi-Fi is gone. Turn Bluetooth back on so the Game Day app can find the
  # panel and hand it new credentials (a changed router password is the
  # common case, and there is no other way back in), and say so on the
  # screen instead of leaving a stale score up. boot_check's poll repaints
  # the normal lines by itself once Wi-Fi returns, so a router reboot
  # resolves with no reboot of our own.
  on_disconnect:
    - ble.enable:
    - script.execute: boot_wifi_lost
    - lambda: |-
        ESP_LOGW("wifi", "Disconnected. Internal heap free: %u bytes",
                 (unsigned) heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
```

- [ ] **Step 4: Add the internal heap sensor**

In `firmware/gameday-common.yaml`, in the `sensor:` block, directly after the
`Free Heap (PSRAM)` entry, add:

```yaml
  - platform: template
    name: "Free Heap (internal)"
    device_class: data_size
    state_class: measurement
    entity_category: diagnostic
    unit_of_measurement: "KiB"
    update_interval: 30s
    lambda: |-
      #include "esp_heap_caps.h"
      return (float) heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024;
```

Note: no `disabled_by_default`, unlike the PSRAM sensor. The spec asks for this
one to be on by default so the v1.3.4 failure mode is visible without anyone
enabling anything.

- [ ] **Step 5: Verify it configures**

Run: `~/development/tools/esphome-venv/bin/esphome config firmware/gameday.yaml >/dev/null && echo OK`
Expected: `OK`. If it fails on `boot_wifi_lost` not being found, the `wifi:`
block is being parsed before `pages/boot.yaml` is included; move the
`script.execute` into a `lambda` calling `id(boot_wifi_lost).execute();`.

- [ ] **Step 6: Commit**

```bash
git add firmware/gameday-common.yaml firmware/pages/boot.yaml
git commit -F- <<'MSG'
Say when Wi-Fi is lost, and let the app back in

The panel kept drawing the last score with the ticker scrolling when
Wi-Fi was gone, so it looked alive when it was not. Worse, Bluetooth was
switched off on the first connect and nothing ever turned it back on, so
a customer whose router password changed could not re-run setup at all.

Turn Bluetooth back on when Wi-Fi drops and put "Wi-Fi lost, open the
Game Day app" on the screen. Improv advertises on its own while Wi-Fi is
down, so the app's setup screen finds the panel with no app change.

Bluetooth coming back is what caused the v1.3.4 reboot loop, so add a
Free Heap (internal) sensor, on by default, and log the internal heap on
both transitions.
MSG
```

---

## Task 8: Reboot after ten minutes with no Wi-Fi (spec part 1, second half)

**Files:**
- Modify: `firmware/gameday-common.yaml` (`globals:`, `interval:`)

- [ ] **Step 1: Add the offline counter**

In the `globals:` block, after `gd_last_opp_logo`, add:

```yaml
  - id: gd_offline_minutes
    type: int
    restore_value: no
    initial_value: '0'
```

- [ ] **Step 2: Add the interval**

Add a top-level `interval:` block to `firmware/gameday-common.yaml` (there is
none today; put it directly after the `globals:` block):

```yaml
interval:
  # A panel that has a saved network but cannot get back on it is stuck in a
  # way nothing else recovers from. Reboot after ten minutes offline. Not
  # armed on a fresh unit, which is legitimately sitting in setup mode with
  # no network to fail to reach.
  - interval: 60s
    then:
      - lambda: |-
          auto *w = wifi::global_wifi_component;
          if (w == nullptr) return;
          if (w->is_connected() || !w->has_sta()) {
            id(gd_offline_minutes) = 0;
            return;
          }
          id(gd_offline_minutes)++;
          ESP_LOGW("wifi", "Offline for %d minute(s)", id(gd_offline_minutes));
          if (id(gd_offline_minutes) >= 10) {
            ESP_LOGW("wifi", "Ten minutes with no Wi-Fi, restarting");
            id(gd_offline_minutes) = 0;
            App.safe_reboot();
          }
```

- [ ] **Step 3: Verify it configures**

Run: `~/development/tools/esphome-venv/bin/esphome config firmware/gameday.yaml >/dev/null && echo OK`
Expected: `OK`

- [ ] **Step 4: Commit**

```bash
git add firmware/gameday-common.yaml
git commit -F- <<'MSG'
Restart after ten minutes with no Wi-Fi

A panel with a saved network that cannot get back on it has no other way
out. Count the minutes offline and safe_reboot at ten. A fresh unit with
no saved network is left alone: it is sitting in setup mode on purpose.
MSG
```

---

## Task 9: Live modes fall back to your team (spec part 7)

"Live college" on an NFL Sunday means "a live college game or nothing", and the
customer gets nothing. Show their saved team's card instead, and keep scanning.

This is the largest change in the release. It adds one state flag, a fallback
branch in the scan handler, ticker text, and a state-document key.

**Files:**
- Modify: `components/gameday/gameday.h` (new member)
- Modify: `components/gameday/gameday.cpp` (`apply_job_` scan branch, `start_job_` live branch, `emit_`, `rebuild_state_`)

- [ ] **Step 1: Add the fallback flag**

In `components/gameday/gameday.h`, next to `live_none_`, add:

```cpp
  bool live_fallback_{false};  // a live mode with no games, showing the saved team
```

- [ ] **Step 2: Enter the fallback when a scan comes back empty**

In `apply_job_()`, replace the whole `if (pick.empty())` body:

```cpp
    if (pick.empty()) {
      ESP_LOGI(TAG, "No live games right now");
      this->live_none_ = true;
      this->schedule_ = Schedule{};
      this->game_ = GameSnapshot{};
      this->prev_ = GameSnapshot{};
      this->misses_ = 0;
      this->emit_({});
      this->schedule_next_(NO_LIVE_RESCAN);
      return;
    }
```

with:

```cpp
    if (pick.empty()) {
      this->live_none_ = true;
      const ::espn::Team *mine = this->current_team_();
      if (mine != nullptr) {
        // Never show an empty board. Fall back to the saved team's card and
        // poll it on the My team path until a scan finds a live game.
        if (!this->live_fallback_) {
          ESP_LOGI(TAG, "No live games, falling back to %s", mine->abbr);
          this->live_fallback_ = true;
          this->live_started_ms_ = now == 0 ? 1 : now;  // starts the rescan clock
          this->schedule_ = Schedule{};
          this->schedule_fetched_ms_ = 0;
          this->game_ = GameSnapshot{};
          this->prev_ = GameSnapshot{};
        }
        this->mark_good_poll_();
        this->schedule_next_(0);  // the My team path fetches the card now
        return;
      }
      ESP_LOGI(TAG, "No live games right now");
      this->live_fallback_ = false;
      this->schedule_ = Schedule{};
      this->game_ = GameSnapshot{};
      this->prev_ = GameSnapshot{};
      this->mark_good_poll_();
      this->emit_({});
      this->schedule_next_(NO_LIVE_RESCAN);
      return;
    }
```

- [ ] **Step 3: Leave the fallback when a scan finds a game**

In the same branch, directly after `this->live_none_ = false;`, add:

```cpp
    this->live_fallback_ = false;
```

- [ ] **Step 4: Run the My team path while in the fallback, and rescan on a timer**

In `start_job_()`, replace the entire live-mode branch, from `if (this->live_mode_()) {`
down to and including its closing `}`, with:

```cpp
  if (this->live_mode_()) {
    // In the fallback, live_started_ms_ times the rescan rather than the
    // current game. Between rescans we drop out of this branch entirely and
    // take the My team schedule/game path below, so the card stays fresh.
    bool rescan_due = this->live_started_ms_ == 0 || (now - this->live_started_ms_) >= NO_LIVE_RESCAN;
    if (!this->live_fallback_ || rescan_due) {
      bool rotate = this->live_started_ms_ != 0 &&
                    (now - this->live_started_ms_) >= (uint32_t) this->prefs2_.rotate_minutes * MINUTE;
      bool ended = this->game_.valid && this->game_.state != GameState::IN;
      if (this->live_fallback_) {
        j.need_scan = true;
        this->live_started_ms_ = now == 0 ? 1 : now;
      } else {
        j.need_scan = !this->schedule_.valid || rotate || ended || this->live_none_;
      }
      j.schedule = this->schedule_;
      this->job_done_ = false;
      this->busy_ = true;
      this->busy_since_ms_ = millis() == 0 ? 1 : millis();
      BaseType_t ok = xTaskCreate(&GamedayComponent::worker_, "gameday_fetch", 16384, this, 1, nullptr);
      if (ok != pdPASS) {
        this->busy_ = false;
        this->schedule_next_(RETRY_INTERVAL);
      }
      return;
    }
    // Fallback, no rescan due: fall through to the My team path below.
  }
```

Note there is no `return;` on the fall-through path, which is the point: the
`j.need_schedule = ...` line that follows is the My team path, and the fallback
card is polled by it exactly as My team mode would.

- [ ] **Step 5: Say so in the ticker**

In `emit_()`, replace:

```cpp
  if (this->live_mode_() && (!g.valid || g.state == GameState::NOT_FOUND))
    f.status_text = this->live_none_ ? "No live games right now" : "Looking for a live game";
```

with:

```cpp
  if (this->live_mode_() && this->live_fallback_) {
    const ::espn::Team *mine = this->current_team_();
    const char *what = this->prefs2_.mode == (uint8_t) Mode::LIVE_NCAA   ? "college"
                       : this->prefs2_.mode == (uint8_t) Mode::LIVE_NFL ? "NFL"
                                                                        : "";
    std::string lead = std::string("No live ") + (what[0] ? std::string(what) + " " : "") + "games";
    if (mine != nullptr)
      lead += std::string(", showing ") + mine->abbr;
    f.status_text = lead + " | " + f.status_text;
  } else if (this->live_mode_() && (!g.valid || g.state == GameState::NOT_FOUND)) {
    const char *what = this->prefs2_.mode == (uint8_t) Mode::LIVE_NCAA   ? " college"
                       : this->prefs2_.mode == (uint8_t) Mode::LIVE_NFL ? " NFL"
                                                                       : "";
    f.status_text = this->live_none_ ? std::string("No live") + what + " games right now"
                                     : std::string("Looking for a live") + what + " game";
  }
```

- [ ] **Step 6: Publish the flag**

In `rebuild_state_()`, after `doc["stale_s"] = this->stale_seconds_();`, add:

```cpp
  doc["fallback"] = this->live_fallback_;
```

- [ ] **Step 7: Clear the flag on any reset**

`reset_game_()` (`gameday.cpp:703`) is the one place every mode change, team
change and reset already funnels through. Add to it, next to `live_none_`:

```cpp
  this->live_none_ = false;
  this->live_fallback_ = false;
```

- [ ] **Step 8: Verify it configures and the host tests still pass**

Run: `make -C tests && ~/development/tools/esphome-venv/bin/esphome config firmware/gameday.yaml >/dev/null && echo OK`
Expected: `191 checks, 0 failures` then `OK`.

- [ ] **Step 9: Commit**

```bash
git add components/gameday/gameday.h components/gameday/gameday.cpp
git commit -F- <<'MSG'
Live modes fall back to your team instead of an empty board

Field report: the panel sat in Live college on an NFL Sunday showing
nothing useful, because the mode meant "a live college game or nothing".

An empty scan now shows the saved team's card, live if they are playing
and the next-game card otherwise, with "No live college games, showing
DAL" leading the ticker and fallback in the state document. The scan
keeps running every two minutes and the first game it finds takes the
board back. A panel with no team saved keeps the old text.

The no-game text now names the league too.
MSG
```

---

## Task 10: README states the network trade-off (spec "Why", last line)

The spec closes its Why section with: "No security changes: the panel trusts the
home network, and that stays as it is (say so in the README)." The README says
nothing about it today. Per the 2026-09-12 audit re-cut this is stated once, in
the owner's words, and never raised again.

**Files:**
- Modify: `README.md` (new subsection under `## Notes`)

- [ ] **Step 1: Add the paragraph**

In `README.md`, at the end of the `## Notes` section, add:

```markdown
### On your network

The panel is a score display, and it treats your home network as trusted.
Anyone already on your Wi-Fi can open its page and change the team, and
updates are not password protected. That is a deliberate trade: it keeps
setup to one tap in the app and keeps the panel working when your phone
is the only thing that knows it exists. It is not designed to be exposed
to the internet, so do not forward a port to it.
```

- [ ] **Step 2: Commit**

```bash
git add README.md
git commit -m "README says the panel trusts the home network"
```

---

## Task 11: Version and changelog

**Files:**
- Modify: `firmware/gameday-common.yaml:8`
- Modify: `CHANGELOG.md`

- [ ] **Step 1: Bump the version**

In `firmware/gameday-common.yaml`, change `version: "1.3.8"` to `version: "1.4.0"`.

- [ ] **Step 2: Add the changelog section**

In `CHANGELOG.md`, directly after the `Format:` paragraph and before `## v1.3.8`, add:

```markdown
## v1.4.0

- The panel now says "Wi-Fi lost, open the Game Day app" instead of leaving an old score on the screen, and Bluetooth switches back on so the app can hand it a new password. A panel that cannot get back on Wi-Fi restarts itself after ten minutes.
- When scores stop updating the ticker says "no update for 4 min" instead of a bare asterisk.
- A college team in a favorite slot now finds its games. It was asking the NFL scoreboard.
- The boot screen shows an address you can actually type, and holding the boot button warns you before it erases your Wi-Fi, with five seconds to let go.
- "Switch every" can be set as low as 1 minute.
- The panel no longer sits on last week's game after a night with the display off.
- The live modes show your team instead of an empty board when no game is on, and go back to live games the moment one starts.
```

- [ ] **Step 3: Verify the tag/section consistency**

Run: `grep -n 'version: "1.4.0"' firmware/gameday-common.yaml && grep -n '^## v1.4.0' CHANGELOG.md`
Expected: one hit each. A tag without a matching section fails CI.

- [ ] **Step 4: Final full check**

Run: `make -C tests && ~/development/tools/esphome-venv/bin/esphome config firmware/gameday.yaml >/dev/null && echo OK`
Expected: `191 checks, 0 failures` then `OK`.

- [ ] **Step 5: Commit**

```bash
git add firmware/gameday-common.yaml CHANGELOG.md
git commit -m "v1.4.0"
```

---

## Handing it to Brandon

Do not claim any part works. The firmware compiles and the host tests pass;
that is all this plan can establish. Brandon runs the bench list from the
spec's Testing section, checks (a) through (k):

- (a) change the router password, panel shows Wi-Fi lost within 60 s, app setup finds it over Bluetooth, joins, normal screen returns
- (b) reboot the router only, panel recovers on its own with no reboot
- (c) block ESPN during a game, ticker says "no update for N min" within 3 polls, clears when unblocked
- (d) leave Wi-Fi off 11 min, panel reboots
- (e) Alabama in slot 1 shows a game
- (f) hold the button 2 s, 6 s and 11 s and see each stage
- (g) internal heap through (a) and (b) stays above 40 KB
- (h) a team that played yesterday shows the next game card within one poll
- (i) Live college on a Sunday shows the DAL next-game card with the note
- (j) Live NFL on a Saturday the same
- (k) Live college on a Saturday morning, DAL card replaced by the first live game

Build and flash: `esphome run firmware/gameday.yaml --device /dev/cu.usbmodem1101`,
or stream the panel's log with
`esphome logs firmware/gameday.yaml --device gameday-2f6a70.local`.
Brandon merges, tags and flashes.
