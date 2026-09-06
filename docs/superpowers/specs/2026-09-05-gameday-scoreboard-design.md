# Game Day Scoreboard, standalone firmware design

Date: 2026-09-05. Status: approved.

## Goal

A standalone football scoreboard for HUB75 matrices on the Apollo M-1. No Home
Assistant, no blueprint. A person flashes a prebuilt binary from a browser,
joins the device to WiFi with Improv, opens the device's web page, picks a
league and team, and the panel tracks that team's games from ESPN by itself.

Ported from the Home Assistant version at bharvey88/gameday-matrix (blueprint
Football Game Day v21 plus the Team Tracker Live page on the hub75-studio
fork, branch gameday). Light strips, RGB bulbs, HyperHDR and every other
celebration output stay out. Matrix plus NFL and college football only.

## Decisions taken

| Question | Decision |
| --- | --- |
| Data source | Device fetches ESPN's public site API directly |
| Logos | ESPHome online_image decodes the ESPN dark-background PNG on device, resized to 32x32 |
| Firmware base | ESPHome on the hub75-studio packages (fork branch gameday until upstream merges) |
| Install | GitHub Pages flasher (ESP Web Tools + Improv), then the device web page |
| Hardware | Apollo M-1 only, two bins: one 64x64 panel, two panels wide (128x64) |
| Repo | New public repo bharvey88/gameday-scoreboard |
| Idle screen | Next-game card |
| Game logic | Score splashes, opponent splashes (switchable), victory splash, composable ticker |
| College scope | All FBS teams |
| Timezone | Dropdown on the device web page, default US Central |
| Home Assistant | The native API stays on so HA users can adopt the device; nothing requires it |

## Repository layout

```
gameday-scoreboard/
  README.md
  LICENSE                         MIT
  firmware/
    gameday-64x64.yaml            panel substitutions only, includes common
    gameday-128x64.yaml           panel substitutions only, includes common
    gameday-common.yaml           packages, wifi/improv/ota/web_server, controls
    pages/gameday-live.yaml       the scoreboard page (LVGL)
  components/gameday/             ESPHome external component (C++)
    __init__.py
    gameday.h / gameday.cpp       poller, fetch, state machine, entity glue
    espn_parse.h / espn_parse.cpp pure parsing and game logic, host-testable
    teams.h                       generated team table
  scripts/build_teams.py          regenerates teams.h from ESPN
  tests/
    fixtures/*.json               recorded ESPN scoreboard and team documents
    test_parse.cpp                host test binary (g++ + ArduinoJson)
    Makefile
  .github/workflows/build.yml     compile both bins on tag, publish Pages
  docs/                           flasher page, manifests filled in by CI
```

## Firmware configuration

`gameday-common.yaml` carries what the current example M-1 config carries,
minus the media proxy: esphome-version, the M-1 rev6 controller package,
utils, theme, bios page, clock page, plus the local live page and the local
external component. WiFi uses `ap:` plus `captive_portal:` and `improv_serial:`
so the flasher can provision credentials. `web_server: version: 3` gives the
control page. `api:` stays on with no encryption key (people who adopt in HA
can add one by building from YAML). `ota: platform: esphome` for updates from
the flasher or ESPHome. `time: platform: sntp` with a timezone set at runtime
from the Timezone select.

The two variant files set `display_layout_cols` to 1 or 2 and nothing else.

## External component `gameday`

A `PollingComponent` with a short internal tick (1 second) that decides when
to actually fetch based on the current phase. It depends on `http_request`
and `json` (for ArduinoJson) and exposes:

- Configuration from YAML: the two `online_image` ids, the four ticker
  switches, the opponent-splash switch, the timezone select, the team select
  and league select, and the two text sensors (status, last play).
- A callback (`on_update`) fired with the 15 fields the old `teamtracker_update`
  API action took: `game_state, team_abbr, team_score, opponent_abbr,
  opponent_score, team_logo, opponent_logo, possession, team_timeouts,
  opponent_timeouts, team_record, opponent_record, status_text, splash_text,
  splash_color`. YAML wires this callback to the page's existing update script.
- A `refresh_now()` method for the button.

### Phases and cadence

| Phase | Condition | Fetch | Interval |
| --- | --- | --- | --- |
| schedule | no known event, or the last one is final and lingered out | team endpoint | 6 hours, plus on boot and on team change |
| pre_far | kickoff more than 60 minutes away | scoreboard | 15 minutes |
| pre_near | kickoff within 60 minutes | scoreboard | 60 seconds |
| in | ESPN state `in` | scoreboard | 10 seconds |
| post | ESPN state `post`, first 30 minutes | scoreboard | 60 seconds |

After the 30 minute linger the phase returns to schedule. A team change resets
everything and fetches the schedule immediately.

### Requests

Team endpoint:
`https://site.api.espn.com/apis/site/v2/sports/football/{nfl|college-football}/teams/{id}`
(about 25KB NFL, about 50KB college). Parsed with a filter that keeps
`team.color`, `team.alternateColor`, `team.record.items[0].summary`,
`team.groups.id`, and `team.nextEvent[0]` (`id`, `date`,
`competitions[0].competitors[].team.{id,abbreviation,logos[0].href}`,
`competitions[0].competitors[].homeAway`, `venue.fullName`,
`broadcasts[].media.shortName`).

Scoreboard endpoint:
`https://site.api.espn.com/apis/site/v2/sports/football/nfl/scoreboard`
(the current week, about 250KB) or
`https://site.api.espn.com/apis/site/v2/sports/football/college-football/scoreboard?groups={conference}&dates={YYYYMMDD}`
(about 100KB). Read through `HttpContainer::read()` in chunks into an
ArduinoJson `deserializeJson` with a filter document so only these survive
for every event: `id`, `status.{displayClock,period,type.{state,completed,detail,shortDetail}}`,
`competitions[0].situation.{lastPlay.text,downDistanceText,possession,homeTimeouts,awayTimeouts}`,
`competitions[0].competitors[].{id,homeAway,score,winner,records[0].summary,team.{abbreviation,color,alternateColor,logo}}`,
`competitions[0].odds[0].{details,overUnder}`, `competitions[0].broadcasts[0].names[0]`,
`competitions[0].venue.fullName`, `competitions[0].date`. The event matching the
known id is copied into the snapshot struct; everything else is discarded.

Logo URLs are rewritten from `/500/` to `/500-dark/` (black logos vanish on a
black panel otherwise). The `online_image` components get `set_url` and
`update` only when the URL changes.

### Snapshot to fields

- `game_state`: `PRE`, `IN`, `POST`, or `NOT_FOUND`.
- `possession`: 1 when the situation's possession id equals our team id, 2 for
  the opponent, 0 otherwise.
- `status_text`: PRE shows the kickoff as "Sat 2:30 PM" in the selected
  timezone (or "Today 2:30 PM", "Tomorrow 2:30 PM"); IN shows
  `shortDetail` (for example "5:36 - 3rd", "Halftime"); POST shows "Final".
- Ticker text follows the blueprint: IN = enabled parts among down and
  distance, clock, last play (truncated to 160 chars), joined by " | ";
  halftime shows records only (win probability is not in the scoreboard
  document); PRE = kickoff, odds, over/under, TV, venue; POST = final line
  with records.

### Splashes

Deltas on our score: 6, 7, 8 = "TOUCHDOWN!", 3 = "FIELD GOAL!", 1 = "EXTRA
POINT!", 2 = "2-POINT!", any other positive delta = "SCORE!". Color = our
primary team color. Opponent deltas mirror with the opponent abbreviation
("IU TOUCHDOWN") in the opponent's color, only while the opponent-splash
switch is on. Victory: once, when the state turns POST with our score ahead,
"{ABBR} WINS!" in our color. Deltas are only evaluated between two IN
snapshots so a boot mid-game never fires a splash. The page's existing
`tt_show_splash` and its 4 second auto-hide are reused unchanged.

### Failure handling

- HTTP failure or parse failure: keep the last snapshot, count a miss. After
  three consecutive misses the status text gets a trailing "*" so the person
  can see it is stale. A successful fetch clears it.
- No next event from the team endpoint: `game_state = NOT_FOUND`, the page
  shows the team logo and "No upcoming game", next schedule fetch in 6 hours.
- WiFi down: nothing fetches; the page keeps whatever it has.
- ESPN response size does not matter because nothing is buffered beyond the
  read chunk and the filtered document (sized 16KB in PSRAM).

## Page `firmware/pages/gameday-live.yaml`

A copy of `packages/pages/teamtracker-live.yaml` from the fork with:

- `ddp_canvas`, `media_proxy_control`, and the two canvas widgets removed.
- Two `online_image` entries (`type: RGB565`, `resize: 32x32`, `format: PNG`,
  `update_interval: never`) drawn with two LVGL `image` widgets in the same
  positions the canvases used, so the adaptive layout script only changes ids.
- The `teamtracker_update` API action becomes a plain script with the same
  15 parameters; the component's callback calls it.
- Everything else (layout measurement, ticker timing, scroll speed
  subscription, splash) unchanged.

The next-game card is the PRE layout the page already renders when
`game_state == "PRE"` (kickoff in the status slot, the pre-game ticker at the
bottom).

## Controls

| Entity | Type | Options / notes |
| --- | --- | --- |
| Team | select | One combined list, "NFL: Dallas Cowboys" and "NCAAF: Boise State Broncos" entries from the generated table, options set at boot by the component; a change restarts the poller; the stored preference is league plus ESPN id |
| Timezone | select | US Eastern, Central, Mountain, Arizona, Pacific, Alaska, Hawaii, UTC, UK, Central Europe |
| Ticker: game clock | switch | default on |
| Ticker: down and distance | switch | default on |
| Ticker: last play | switch | default on |
| Ticker: pre-game odds and TV | switch | default on |
| Opponent splashes | switch | default on |
| Refresh now | button | |
| Game status | text_sensor | the status text |
| Last play | text_sensor | |
| Scroll Speed | number | already provided by utils.yaml |

## Team table

`scripts/build_teams.py` fetches the NFL team list and, for college, the
scoreboard for each FBS conference group over a few weeks of the season to
collect every FBS team id, abbreviation, display name and conference group.
Output is `components/gameday/teams.h` as a constexpr array sorted by display
name. The script is run by a person at release time and the header is
committed, so a build never needs network access.

## Build and distribution

- `build.yml` on a `v*` tag: compile both variants with the ESPHome version
  pinned in the fork's esphome-version package, upload the bins, write
  `docs/manifest-64x64.json` and `docs/manifest-128x64.json` for ESP Web
  Tools, and deploy `docs/` to GitHub Pages. A pull request builds both
  variants without publishing.
- `docs/index.html`: two install buttons (one per layout), Improv WiFi
  provisioning built into ESP Web Tools, then a short "open the device page
  and pick your team" walkthrough. Layout follows the apollo-installer page.
- README: what it does, hardware needed, the three install steps, how to add
  the device to Home Assistant if wanted, how to build from YAML.

## Testing

- `tests/test_parse.cpp` compiles on the host with g++ and the ArduinoJson
  single header, against fixtures recorded from ESPN: NFL pre, college in,
  college post, a halftime status, a game with odds and TV, and a scoring
  sequence (two snapshots per delta type). It asserts the snapshot fields,
  the splash decisions, and the ticker text.
- `esphome compile` of both variants from `C:\Users\bharv\esphome-venv`.
- On-device (Brandon flashes): logo PNG decode over TLS, PSRAM headroom during
  the NFL scoreboard fetch, layout on 1 and 2 panels, a real live game.

## Out of scope

Lights and celebrations beyond the matrix, other sports, win probability,
multi-team priority, controllers other than the M-1, a wiki tutorial.
