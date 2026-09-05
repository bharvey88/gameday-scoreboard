# Game Day Scoreboard Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Standalone ESPHome firmware for the Apollo M-1 that shows a football scoreboard for one chosen NFL or FBS team, fed directly from ESPN, with a browser flasher for installation.

**Architecture:** A C++ ESPHome external component (`gameday`) polls ESPN through `http_request`, streams the JSON through an ArduinoJson filter into a small snapshot struct, runs the pre/in/post state machine and splash logic, and fires a 15-field callback. A copied hub75-studio LVGL page renders those fields; two `online_image` components fetch the logos. Template entities in YAML give a web-page UI (team, timezone, ticker toggles) with the component holding the persisted preferences.

**Tech Stack:** ESPHome 2026.8.x (esp-idf, ESP32-S3), hub75-studio packages from bharvey88/hub75-studio branch gameday, ArduinoJson 7.4.3, LVGL, ESP Web Tools + Improv, GitHub Actions + Pages, Python 3 for the team table script, g++ for host tests.

**Spec:** `docs/superpowers/specs/2026-09-05-gameday-scoreboard-design.md`

## Global Constraints

- Public repo bharvey88/gameday-scoreboard, MIT license. No Claude credit anywhere. Commit email `8107750+bharvey88@users.noreply.github.com`.
- No em dashes in any text. Markdown hyperlinks, never raw URLs, in prose.
- Hardware only: Apollo M-1 rev6. Two variants: `gameday-64x64` (cols 1) and `gameday-128x64` (cols 2).
- Logo URLs rewritten `/500/` to `/500-dark/`.
- Poll cadence: schedule 6h; pre_far 15m; pre_near (kickoff within 60m) 60s; in 20s; post 60s for 30m.
- Splash deltas: 6/7/8 TOUCHDOWN!, 3 FIELD GOAL!, 1 EXTRA POINT!, 2 2-POINT!, other positive SCORE!. Opponent form: "{OPP} TOUCHDOWN" etc. Victory: "{ABBR} WINS!". Deltas only between two IN snapshots.
- Stale marker: trailing "*" on status text after 3 consecutive fetch misses.
- Flashing and serial monitoring are Brandon's job. Claude compiles from `C:\Users\bharv\esphome-venv`.
- One combined Team select ("NFL: Dallas Cowboys", "NCAA: Boise State Broncos") instead of League + Team; the spec's Controls table is amended in Task 1.

---

## File map

| File | Responsibility |
| --- | --- |
| `components/gameday/espn_parse.h/.cpp` | Pure functions: filtered parse of team and scoreboard JSON into `Schedule` and `GameSnapshot`; splash decision; ticker and status text; URL builders. No ESPHome includes, host-testable. |
| `components/gameday/gameday.h/.cpp` | `GamedayComponent`: phase timer, HTTP fetch via `HttpContainer` streaming reader, preferences, callback, entity glue. |
| `components/gameday/teams.h` | Generated `constexpr Team kTeams[]`. |
| `components/gameday/__init__.py` | Component schema: `http_request_id`, `time_id`, `on_update` trigger. |
| `scripts/build_teams.py` | Generates `teams.h` from ESPN. |
| `tests/` | Fixtures, `test_parse.cpp`, `Makefile`. |
| `firmware/gameday-common.yaml` | Base config and template entities. |
| `firmware/gameday-64x64.yaml`, `firmware/gameday-128x64.yaml` | Panel substitutions. |
| `firmware/pages/gameday-live.yaml` | LVGL page. |
| `.github/workflows/build.yml`, `docs/` | CI and flasher. |
| `README.md` | Docs. |

---

### Task 1: Repo skeleton, license, README stub, spec amendment

**Files:**
- Create: `LICENSE` (MIT, "Copyright (c) 2026 Brandon Harvey"), `README.md` (title and one-paragraph description only), `.gitignore` (`.esphome/`, `firmware/.esphome/`, `*.bin`, `tests/build/`, `secrets.yaml`)
- Modify: `docs/superpowers/specs/2026-09-05-gameday-scoreboard-design.md` Controls table: replace the League and Team rows with one row `Team | select | "NFL: Dallas Cowboys" and "NCAA: ..." entries from the generated table, options set at boot by the component, preference stored as league plus ESPN id`.

- [ ] **Step 1:** Write the three files and amend the spec.
- [ ] **Step 2:** `git add -A && git commit -m "Repo skeleton"`.

---

### Task 2: Team table generator

**Files:**
- Create: `scripts/build_teams.py`, `components/gameday/teams.h` (generated and committed)

**Interfaces:**
- Produces `components/gameday/teams.h`:

```cpp
#pragma once
#include <cstdint>
namespace gameday {
enum class League : uint8_t { NFL = 0, NCAA = 1 };
struct Team {
  League league;
  uint32_t espn_id;
  const char *abbr;      // "DAL"
  const char *name;      // "Dallas Cowboys"
  uint32_t group;        // conference group id for the college scoreboard; 0 for NFL
};
constexpr Team kTeams[] = { /* sorted by league then name */ };
constexpr size_t kTeamCount = sizeof(kTeams) / sizeof(kTeams[0]);
}  // namespace gameday
```

- [ ] **Step 1:** Write `scripts/build_teams.py`:
  - NFL: GET `https://site.api.espn.com/apis/site/v2/sports/football/nfl/teams?limit=100`, read `sports[0].leagues[0].teams[].team.{id,abbreviation,displayName}`.
  - NCAA: for `week` 1..15, GET `https://site.api.espn.com/apis/site/v2/sports/football/college-football/scoreboard?groups=80&limit=400&week=N`, collect `competitions[0].competitors[].team.{id,abbreviation,displayName,conferenceId}`; keep teams whose `conferenceId` is in the FBS set `{1,4,5,8,9,12,15,17,18,37,151}`. Union across weeks, dedupe by id.
  - Emit `teams.h` exactly as above, names escaped, sorted (NFL first, then by name).
  - Print counts. Fail if NFL count != 32 or NCAA count < 120.
- [ ] **Step 2:** Run `python scripts/build_teams.py`; verify counts printed (32 NFL, about 136 NCAA) and `teams.h` exists.
- [ ] **Step 3:** Commit: `git commit -m "Team table generator and generated table"`.

---

### Task 3: Parser and game logic (host-tested)

**Files:**
- Create: `components/gameday/espn_parse.h`, `components/gameday/espn_parse.cpp`, `tests/test_parse.cpp`, `tests/Makefile`, `tests/fixtures/*.json`, `tests/ArduinoJson.h` (single-header 7.4.3 downloaded from the release, committed).

**Interfaces (produces):**

```cpp
// espn_parse.h  (no ESPHome dependencies)
#pragma once
#include <string>
#include <cstdint>
#include "teams.h"
namespace gameday {

enum class GameState : uint8_t { NOT_FOUND, PRE, IN, POST };

struct Schedule {          // from the team endpoint
  bool valid{false};
  std::string event_id;
  int64_t kickoff_epoch{0};   // parsed from "2026-09-05T19:30Z"
  uint32_t group{0};          // team.groups.id (college)
  std::string team_color;     // "0000ff" (no #)
  std::string team_record;    // "3-0"
};

struct GameSnapshot {      // from one scoreboard event
  bool valid{false};
  GameState state{GameState::NOT_FOUND};
  std::string event_id;
  int64_t kickoff_epoch{0};
  bool completed{false};
  std::string short_detail;   // "5:36 - 3rd", "Halftime", "Final"
  std::string display_clock;
  int period{0};
  // ours / theirs resolved against our team id
  std::string team_abbr, opp_abbr;
  int team_score{0}, opp_score{0};
  std::string team_record, opp_record;
  std::string team_color, opp_color;   // hex without #
  std::string team_logo, opp_logo;     // already rewritten to /500-dark/
  bool team_winner{false};
  int possession{0};                   // 0 none, 1 ours, 2 theirs
  int team_timeouts{0}, opp_timeouts{0};
  std::string last_play, down_distance;
  std::string odds, over_under, tv, venue;
};

struct TickerOptions { bool clock{true}, down_distance{true}, last_play{true}, odds{true}; };

struct Splash { std::string text; uint32_t color{0}; };  // empty text = none

// URL builders
std::string team_url(League league, uint32_t espn_id);
std::string scoreboard_url(League league, uint32_t group, int64_t kickoff_epoch);  // college adds groups and dates=YYYYMMDD
std::string dark_logo(const std::string &url);   // "/500/" -> "/500-dark/"

// Parsing. `reader` is any ArduinoJson-compatible reader (Stream-like object
// with int read() and size_t readBytes(char*, size_t)); the JSON is consumed
// with a filter so memory stays small. Returns false on parse error.
template<typename TReader> bool parse_team(TReader &reader, Schedule &out);
template<typename TReader> bool parse_scoreboard(TReader &reader, const std::string &event_id, uint32_t our_team_id, GameSnapshot &out);
// Convenience for strings (tests):
bool parse_team_str(const std::string &json, Schedule &out);
bool parse_scoreboard_str(const std::string &json, const std::string &event_id, uint32_t our_team_id, GameSnapshot &out);

// Logic
Splash decide_splash(const GameSnapshot &prev, const GameSnapshot &cur, bool opponent_splashes);
uint32_t parse_color(const std::string &hex);      // "0000ff" -> 0x0000FF, fallback 0xFFFFFF
std::string status_text(const GameSnapshot &s, const TickerOptions &o, const std::string &kickoff_local);
// kickoff_local is "Sat 2:30 PM" / "Today 7:20 PM" produced by the component (needs the TZ).
std::string kickoff_label(int64_t kickoff_epoch, int64_t now_epoch, const struct tm &kick_local, const struct tm &now_local);
int64_t parse_iso8601_z(const std::string &s);     // "2026-09-05T19:30Z" or with seconds
}  // namespace gameday
```

Filter documents (ArduinoJson `JsonDocument filter`): team endpoint keeps `team.color, team.alternateColor, team.record.items[0].summary, team.groups.id, team.nextEvent[0].id, team.nextEvent[0].date`. Scoreboard keeps for `events[*]`: `id, date, status.displayClock, status.period, status.type.state, status.type.completed, status.type.shortDetail, competitions[0].situation.{lastPlay.text,downDistanceText,possession,homeTimeouts,awayTimeouts}, competitions[0].competitors[*].{id,homeAway,score,winner,records[0].summary,team.{abbreviation,color,logo}}, competitions[0].odds[0].{details,overUnder}, competitions[0].broadcasts[0].names[0], competitions[0].venue.fullName`.

Timeouts map: `homeTimeouts` belongs to the competitor whose `homeAway == "home"`.

`decide_splash` rules: only when `prev.valid && prev.state == IN && cur.state == IN && same event_id`: delta = cur.team_score - prev.team_score; 6,7,8 "TOUCHDOWN!", 3 "FIELD GOAL!", 1 "EXTRA POINT!", 2 "2-POINT!", >0 other "SCORE!", color = parse_color(team_color). Opponent (if enabled and no own splash): "{OPP} TOUCHDOWN", "{OPP} FIELD GOAL", "{OPP} EXTRA POINT", "{OPP} 2-POINT", "{OPP} SCORE", color = opp color. Victory: `prev.state == IN && cur.state == POST && cur.team_score > cur.opp_score` gives "{ABBR} WINS!".

`status_text`: PRE = parts [kickoff_local, odds, "O/U " + over_under, tv, venue] (odds/O/U/tv only if `o.odds`), non-empty joined " | ". IN and short_detail contains "Halftime" = "Halftime | {team_abbr} {team_record} | {opp_abbr} {opp_record}". IN otherwise = enabled non-empty parts among [down_distance, short_detail (clock), last_play truncated to 160 with "..."], joined " | ", falling back to short_detail. POST = "Final | {team_abbr} {team_record} | {opp_abbr} {opp_record}". NOT_FOUND = "No upcoming game".

- [ ] **Step 1:** Record fixtures with curl into `tests/fixtures/`: `team_nfl_dal.json`, `team_ncaa_bc.json` (id 103), `scoreboard_nfl.json` (current week), `scoreboard_ncaa_live.json` (`groups=80&limit=200` on a Saturday, contains at least one `in` game). Pick one `in` event and one `post` event id from the college file and note them in the test. Also hand-write `synthetic_scores.json`: a minimal scoreboard document with one event in state `in` used to synthesize score sequences by string replacement in the test.
- [ ] **Step 2:** Write `tests/Makefile`: `g++ -std=c++17 -I../components/gameday -I. test_parse.cpp ../components/gameday/espn_parse.cpp -o build/test_parse && ./build/test_parse`.
- [ ] **Step 3:** Write failing tests in `tests/test_parse.cpp` (tiny assert macro, no framework): team parse fields; scoreboard parse of the live college event (abbr, scores, possession, timeouts, last play, dark logo rewrite); a post event (state POST, records, winner); NFL pre event (odds, tv, venue, kickoff epoch); `decide_splash` for every delta and the opponent and victory cases and the no-splash cases (boot mid-game, PRE to IN); `status_text` for PRE, IN (each toggle off), halftime, POST; `scoreboard_url` for both leagues; `parse_iso8601_z`.
- [ ] **Step 4:** `make -C tests` fails to compile (functions undefined).
- [ ] **Step 5:** Implement `espn_parse.cpp`.
- [ ] **Step 6:** `make -C tests` passes.
- [ ] **Step 7:** Commit: `git commit -m "ESPN parser, splash and ticker logic with host tests"`.

---

### Task 4: The `gameday` ESPHome component

**Files:**
- Create: `components/gameday/__init__.py`, `components/gameday/gameday.h`, `components/gameday/gameday.cpp`

**Interfaces (produces):**

```cpp
namespace esphome { namespace gameday {
struct UpdateFields {
  std::string game_state, team_abbr, opponent_abbr, status_text, team_logo, opponent_logo,
              team_record, opponent_record, splash_text;
  int team_score{0}, opponent_score{0}, possession{0}, team_timeouts{0}, opponent_timeouts{0};
  uint32_t splash_color{0};
};
class GamedayComponent : public Component {
 public:
  void set_http(http_request::HttpRequestComponent *h);
  void set_time(time::RealTimeClock *t);
  void set_team_select(select::Select *s);        // options filled at setup
  void set_timezone_select(select::Select *s);    // options filled at setup
  void add_on_update_callback(std::function<void(const UpdateFields &)> cb);
  void set_ticker_clock(bool), set_ticker_down_distance(bool), set_ticker_last_play(bool), set_ticker_odds(bool);
  void set_opponent_splashes(bool);
  bool ticker_clock() const; /* etc, for template switch lambdas */
  void select_team(const std::string &option);     // "NFL: Dallas Cowboys"
  void select_timezone(const std::string &option); // "US Central"
  void refresh_now();
  const std::string &status() const;  // last status text (for text_sensor)
  const std::string &last_play() const;
  void setup() override; void loop() override; void dump_config() override;
  float get_setup_priority() const override { return setup_priority::LATE; }
};
}}
```

Preferences: one struct `{uint8_t league; uint32_t team_id; uint8_t tz_index; uint8_t flags;}` in `global_preferences->make_preference<Prefs>(fnv1_hash("gameday_prefs"))`. Defaults: NFL Dallas Cowboys (id 6), US Central, all flags on. Timezone table: `{"US Eastern","EST5EDT,M3.2.0,M11.1.0"}, {"US Central","CST6CDT,M3.2.0,M11.1.0"}, {"US Mountain","MST7MDT,M3.2.0,M11.1.0"}, {"US Arizona","MST7"}, {"US Pacific","PST8PDT,M3.2.0,M11.1.0"}, {"US Alaska","AKST9AKDT,M3.2.0,M11.1.0"}, {"US Hawaii","HST10"}, {"UTC","UTC0"}, {"UK","GMT0BST,M3.5.0/1,M10.5.0"}, {"Central Europe","CET-1CEST,M3.5.0,M10.5.0/3"}`.

Fetching: in `loop()`, when `millis() - last_fetch_ >= interval_for_phase()` and WiFi is connected and the time is valid, call `fetch_()`. `fetch_()` blocks (the ESPHome http client is synchronous); wrap with `App.feed_wdt()` inside the reader. Reader class:

```cpp
struct ContainerReader {
  std::shared_ptr<http_request::HttpContainer> c; uint8_t buf[1024]; size_t pos{0}, len{0}; bool eof{false};
  int read();                                    // one byte, -1 at end
  size_t readBytes(char *dst, size_t n);
};
```

Phase machine in `fetch_()`: if no schedule or schedule stale (6h) or team changed: fetch team endpoint, set `schedule_`; if no event, state NOT_FOUND, emit, return. Else fetch scoreboard; on parse success: `splash = decide_splash(prev_, cur, opponent_splashes)`, `prev_ = cur`, misses = 0, emit. On failure: misses++, if misses >= 3 re-emit last with "*". Phase from `cur.state` and kickoff distance. POST linger: 30 minutes after first POST then clear schedule so the next loop fetches the team endpoint (which now has the next event).

Emit: build `UpdateFields` (status_text via `status_text()` with `kickoff_label()` using `time_->now()` and `ESPTime::from_epoch_local`), `game_state` string, logos, then call callbacks. Logo handling stays in YAML.

`__init__.py`: `CONFIG_SCHEMA` with `id`, `http_request_id` (use_id HttpRequestComponent), `time_id` (use_id RealTimeClock), `team_select` (use_id select.Select), `timezone_select` (use_id select.Select), `on_update` (automation with `(const UpdateFields&, "x")` trigger type). `DEPENDENCIES = ["http_request", "time", "select", "network"]`, `AUTO_LOAD = ["json"]`.

- [ ] **Step 1:** Write `__init__.py`, `gameday.h`, `gameday.cpp`.
- [ ] **Step 2:** Create a minimal `firmware/gameday-common.yaml` (Task 5 fills it out) with just enough to compile: esphome, esp32 board via the controller package, wifi, http_request, sntp, two template selects, the component, and an `on_update` that logs. Run `esphome compile firmware/gameday-64x64.yaml` from the venv. Fix compile errors.
- [ ] **Step 3:** Commit: `git commit -m "gameday component: ESPN poller, phases, preferences"`.

---

### Task 5: Firmware YAML and the page

**Files:**
- Create: `firmware/gameday-common.yaml`, `firmware/gameday-64x64.yaml`, `firmware/gameday-128x64.yaml`, `firmware/pages/gameday-live.yaml`

`gameday-common.yaml` contents (structure):
- `substitutions`: name `gameday-scoreboard`, friendly_name `Game Day Scoreboard`, panel dims, `display_layout_cols: "1"` default (variant files override), DISPLAY_W/H.
- `esphome`: name, friendly_name, `name_add_mac_suffix: true`, `project: {name: bharvey88.gameday-scoreboard, version: "0.1.0"}`, `min_version` comes from the package.
- `external_components: - source: {type: local, path: ../components}` (relative to the YAML), `components: [gameday]`.
- `packages` from `https://github.com/bharvey88/hub75-studio` ref `gameday`: esphome-version, apollo-automation-m1-rev6 (with panel vars), utils, theme, bios, clock. Plus local `pages/gameday-live.yaml` via `!include`.
- `logger`, `api` (no key), `ota` esphome, `wifi` with `ap:` and `power_save_mode: none`, `captive_portal`, `improv_serial`, `web_server: version: 3`, `http_request: {id: http_client, timeout: 20s, watchdog_timeout: 30s, verify_ssl: false, buffer_size_rx: 2048}` (ESPN's CDN certificate chain is not in the default esp-idf bundle on every build, and verify_ssl false keeps the flasher build small; document this in the README), `time: - platform: sntp, id: sntp_time`.
- `online_image` x2: `id: gd_team_logo` / `gd_opp_logo`, `url: https://a.espncdn.com/i/teamlogos/nfl/500-dark/dal.png` placeholder, `type: RGB565`, `format: PNG`, `resize: 32x32`, `transparency: alpha_channel`, `update_interval: never`, `buffer_size: 4096`, `on_download_finished` calls `lvgl.image.update` for the matching widget.
- `select` template x2: `gd_team` (name "Team", `optimistic: true`, `options: ["Loading"]`, `on_value: lambda id(gameday).select_team(x)`), `gd_timezone` (name "Timezone", options listed inline matching the component table, `initial_option: "US Central"`, `on_value: id(gameday).select_timezone(x)`).
- `switch` template x5 with `lambda: return id(gameday).ticker_clock();` style getters and `turn_on_action`/`turn_off_action` calling setters; `restore_mode: DISABLED` (component owns persistence).
- `button` template "Refresh now" -> `id(gameday).refresh_now()`.
- `text_sensor` template x2 (Game status, Last play) updated from the `on_update` automation.
- `gameday:` block wiring `http_request_id`, `time_id`, `team_select`, `timezone_select`, `on_update` which: sets the two text sensors, calls `script.execute: gd_update` with all 15 fields, and for each logo URL that changed calls `online_image.set_url` + update.

`gameday-live.yaml`: copy of the fork page with these edits: delete `ddp_canvas`, `media_proxy_control`, the two `globals`, the `on_load` lambda's output starts; rename `tt_away_logo_canvas`/`tt_home_logo_canvas` to `gd_team_logo_img`/`gd_opp_logo_img` as `image` widgets with `src: gd_team_logo` / `gd_opp_logo`; convert `api.actions.teamtracker_update` to `script: - id: gd_update, parameters: {...15...}`, dropping the two media_proxy `if` blocks; keep `tt_apply_layout`, `tt_hide_splash`, ticker code, scroll-speed subscription. `on_load` runs `tt_apply_layout` only. Add `lvgl_page_manager` page entry and set it as the page to show at boot after bios (check how the page manager picks the initial page in `packages/common/utils.yaml`; add `initial_page` or the equivalent if it has one, otherwise call the page select in `on_boot`).

- [ ] **Step 1:** Write the four files.
- [ ] **Step 2:** `esphome config firmware/gameday-64x64.yaml` and `esphome config firmware/gameday-128x64.yaml` pass.
- [ ] **Step 3:** `esphome compile` both variants from the venv. Fix errors. Record the flash and RAM summary lines in the commit message body.
- [ ] **Step 4:** Commit with `-F` file: "Firmware configs and the scoreboard page".

---

### Task 6: CI and flasher

**Files:**
- Create: `.github/workflows/build.yml`, `docs/index.html`, `docs/manifest-64x64.json`, `docs/manifest-128x64.json` (templates with `VERSION` placeholder), `docs/style.css`

`build.yml`: triggers `pull_request` and `push: tags: v*`. Job `build` matrix over `[gameday-64x64, gameday-128x64]`: checkout, `esphome/build-action@v7` with `yaml-file: firmware/${{ matrix.variant }}.yaml`, `version: 2026.8.1`, `complete-manifest: true`, `release-summary`, `release-url` set from the tag. Upload artifact. Job `publish` (tags only): download both artifacts into `site/firmware/<variant>/`, copy `docs/*` into `site/`, `sed` VERSION into the manifests, `actions/upload-pages-artifact` + `actions/deploy-pages`. Also create a GitHub Release with the two `.factory.bin` files attached.

`docs/index.html`: ESP Web Tools from cdnjs is not available; use `https://unpkg.com/esp-web-tools@10/dist/web/install-button.js` as a module script (this page runs on GitHub Pages, not in an artifact, so no CSP restriction). Two `<esp-web-install-button manifest="manifest-64x64.json">` buttons with a short three-step walkthrough: connect USB and install, join WiFi in the dialog, open `http://gameday-scoreboard-xxxxxx.local` (or the IP the dialog shows) and choose your team. Note the Chrome/Edge requirement. Style modeled on the apollo-installer page (read `C:\Users\bharv\development\apollo-installer\index.html` and `css/` for tone, do not copy Apollo branding).

- [ ] **Step 1:** Write the workflow, manifests, page.
- [ ] **Step 2:** Validate the workflow with `gh workflow view` after push (Task 8) or `actionlint` if installed; at minimum `python -c "import yaml; yaml.safe_load(open('.github/workflows/build.yml'))"`.
- [ ] **Step 3:** Commit: `git commit -m "CI build for both variants and the Pages flasher"`.

---

### Task 7: README

**Files:**
- Modify: `README.md`

Sections: What it does (bullets from the spec goal), Hardware (M-1 + one or two 64x64 panels), Install (three steps, link to the Pages flasher), Pick your team (web page walkthrough, list of the controls), Home Assistant (optional: it shows up as an ESPHome device; the same controls appear as entities), Build it yourself (clone, `esphome run firmware/gameday-128x64.yaml`, where to change substitutions), How it works (ESPN polling cadence, one paragraph), Notes (verify_ssl off and why, ESPN is unofficial, logos are ESPN's), Credits (hub75-studio project and Team Tracker for the attribute model, by project name not person name), License.

- [ ] **Step 1:** Write it, no em dashes, markdown links only.
- [ ] **Step 2:** Commit: `git commit -m "README"`.

---

### Task 8: Publish the repo and verify CI

- [ ] **Step 1:** `gh repo create bharvey88/gameday-scoreboard --public --source . --push` (run standalone, not chained with cd).
- [ ] **Step 2:** Enable Pages with source GitHub Actions: `gh api -X POST repos/bharvey88/gameday-scoreboard/pages -f build_type=workflow`.
- [ ] **Step 3:** Open a throwaway PR or push a branch to confirm the build job compiles both variants in CI; fix and re-run until green. Do not tag a release; Brandon tags after his flash test.
- [ ] **Step 4:** Write the on-device test checklist into `HANDOFF.md` (gitignored): flash `.factory.bin` from a CI artifact or `esphome run`, watch for logo decode success, heap/PSRAM log lines during the NFL fetch, both layouts, a live game.

---

## Self-review

- Spec coverage: data source and cadence (Task 4), filter fields and parse (Task 3), splashes and ticker (Task 3), page and logos (Task 5), controls (Task 5, Team select amended in Task 1), team table (Task 2), CI and flasher (Task 6), README (Task 7), tests (Task 3 host, Task 5 compile, Task 8 on-device list). Win probability is out of scope per spec.
- Placeholders: none; each code-bearing step names the exact files, signatures and rules.
- Type consistency: `UpdateFields` field names match the page script parameters (`team_abbr, team_score, opponent_abbr, opponent_score, status_text, team_logo, opponent_logo, possession, team_timeouts, opponent_timeouts, team_record, opponent_record, splash_text, splash_color, game_state`); `GameSnapshot` names are used identically in Tasks 3 and 4.
