# Idle content, live-mode fallback and startup (v1.5.0)

Date: 2026-09-23. Status: draft for owner review.

## Why

Two behaviors read as broken on a panel that is meant to be sold:

1. A live mode with nothing live shows the saved team's last game and the
   ticker says "No live college games, showing DAL". The board contradicts
   the mode the owner picked, then apologizes for it. (Shipped on purpose in
   v1.4.0; this spec reverses it.)
2. Boot shows a "----" board with a "Loading" ticker for up to 30 seconds.

And a football panel has nothing to say on a Tuesday night. It needs idle
content that still looks like a product.

## Goals

- A live mode never borrows the saved team. It shows that league's next thing.
- When no mode has anything to show, the panel rotates through idle screens
  the owner chose: clock, countdown, standings, record, weather, or off.
- First real content within about 8 seconds of power-on on a normal network.
  The "Loading" board is gone; the boot screen stays until content exists,
  and the clock is real content.

## Non-goals

- Quotes and any sport beyond NFL and college football (parked for the
  sports expansion).
- Remembering scores across reboots. A stale score never shows.
- Changes to Favorites mode's lock-on logic.

## 1. Live modes with nothing live

Order of precedence, evaluated every scan (2 minutes, unchanged):

1. A live game in the league (unchanged: pick one, rotate as today).
2. Games later today in the league: show the next kickoff as a pre-game
   card (logos, names, records; kickoff, TV and odds in the ticker). Data is
   already in the scan download; the parser currently discards state "pre"
   games (espn_parse_impl.h, keep only "in"). Keep the earliest unstarted
   game too.
3. Today's finals in the league, rotating one per `rotate_minutes`, until
   midnight local time. Data is already in the scan download; keep finals.
4. The next game this week in the league: one extra request,
   `scoreboard?limit=N` with no date (about 25 to 40 KB measured). ESPN
   returns games in kickoff order and includes played ones once the week has
   started, so ask for N=8 and take the first game with state "pre". If none,
   ask again with the next week (`week=` parameter, verify against ESPN).
5. Nothing found (off season): the idle rotation, section 2.

Ticker style, always: "Next college game: BC vs ND, Thu 6:30 PM" (league word
"NFL", "college", or none for Live anything). For step 2: "Next college game:
UGA at BAMA, 2:30 PM". For step 3 the existing final ticker, prefixed with
"Today:". Never "showing DAL", never an apology.

Setting `live_fallback`: `next_game` (default) or `my_team` (the v1.4.0
behavior, kept for owners who liked it). Exposed on the device page under
Setup and in the state document as `fallback_mode`; set key `fallback`.

`live_fallback_` and the "showing DAL" text go away. `our_id_()` in live
modes orients around the away team as before v1.4.0.

## 2. Idle rotation

Entered when the current mode has nothing to show: My team with no game in
the schedule (off season or a bye with nothing until next week is still a
"next game", so idle is rare here), live modes at step 5, Favorites with
every slot empty or nothing scheduled. Also entered by the clock-first rule
at boot (section 3) until the first fetch lands.

Screens, each an LVGL page under the existing page manager so Select Page and
the WizMote page buttons still work:

- **Clock**: time large in the saved team's colors, the next kickoff small
  under it ("DAL vs BAL Sun 3:25"), the two logos at the sides when a next
  game is known. Reuses the hub75-studio clock page's time source.
- **Countdown**: "3d 14h 22m" large, "DAL vs BAL" and TV under it, logos at
  the sides. Ticks every minute; the last hour ticks every second.
- **Standings**: the saved team's division (NFL) or conference (college),
  four rows: rank, abbreviation, W-L. One fetch a day from ESPN's standings
  endpoint (`/standings`, verify the shape and size before building; cap the
  parse with the same ArduinoJson filter approach). If the group has more
  than four teams, rotate pages of four every 20 seconds.
- **Record**: the saved team's record, last result ("W 37-20 vs WSH") and
  next opponent. No extra fetch; all from data the panel already has.
- **Weather**: current temperature, condition word and today's high/low for
  the owner's location, with the clock small. Source: National Weather
  Service (api.weather.gov), no key, US only, public domain. Two requests:
  `/points/{lat},{lon}` once (cache the forecast URL in prefs), then the
  forecast every 30 minutes. Send the required User-Agent. Location is a
  lat/lon pair entered on the device page (Setup card, "Weather location",
  with a "use my phone's location" button that fills it from the browser's
  geolocation) and by the app. No location means the weather screen is
  skipped, not an error.
- **Off after idle**: after `idle_off_minutes` (default off, choices 15, 30,
  60, 120) in idle, brightness goes to 0 and the panel wakes itself when the
  mode has something to show again, or on any user gesture (the v1.4.1
  auto-on rules). Off is not a screen; it is a timer over the rotation.

Rotation: every `idle_rotate_seconds` (default 60), only through screens the
owner enabled. Defaults: Clock and Countdown on, the rest off. Settings live
in a new `Prefs5` (idle flags bitmask, idle_rotate_seconds, idle_off_minutes,
weather lat/lon as two floats, cached forecast URL as a short string; keep
the struct under 96 bytes). State document keys: `idle` (bool),
`idle_screens` (bitmask), `idle_rotate`, `idle_off`, `wx_lat`, `wx_lon`.
Set keys: `idle`, `idlerot`, `idleoff`, `wxlat`, `wxlon`. Device page: an
"When nothing is on" card with a toggle per screen and the two numbers.

Leaving idle: the moment the mode has content, the page manager returns to
the scoreboard page.

## 3. Startup

Target: first real content within about 8 seconds of power-on on a normal
network. Measured, not assumed: the first task is one USB boot log captured
by the owner, read before the fetch changes are written, and a second log
after, quoted in the changelog.

Changes:

1. **Readiness poll**: the fetch loop re-checks Wi-Fi and clock every
   5 seconds (`NOT_READY_INTERVAL`). Make it 500 ms, or trigger on the
   time-sync callback. Saves up to 5 seconds.
2. **Early stop on the league scan**: the FBS Saturday download is about
   1.2 MB. ESPN lists games by kickoff time. Stop reading once the parser has
   passed the last "in" game and seen the first "pre" game, unless section 1
   needs today's finals (it does: keep reading until the first "pre" game,
   which still skips everything after it). Needs a streaming parser hook;
   verify ArduinoJson's filter can abort, otherwise close the socket once the
   needed events are seen.
3. **Cache team info**: My team fetches team info (23 KB) before the
   scoreboard on every boot. Cache the fields the card needs in prefs, keyed
   by league and team id, refreshed once a day. On boot, skip straight to the
   scoreboard.
4. **Boot screen holds**: the boot overlay stays until the first board update
   with content or the idle page is ready, instead of hiding 3 seconds after
   Wi-Fi. The moving bar keeps moving. The "----" and "Loading" placeholders
   are removed from the scoreboard page.
5. **Clock first**: as soon as time is synced and no content has landed, the
   idle Clock page shows (with no next kickoff yet). When the first fetch
   lands, the scoreboard page takes over. This replaces the "Loading" board
   with something true.

Wi-Fi join itself, the Improv/BLE flow, and the setup screens (PR #14 QR
code, PR #16 Wi-Fi scan) are untouched. Those PRs land first; this work
branches from main after them.

## Data and memory

- New ESPN requests: `scoreboard?limit=8` (next game), `/standings`. Both go
  through the existing worker task and http client with the curl user agent.
- NWS: two endpoints, JSON, small. verify_ssl stays on (Let's Encrypt).
- Internal heap is about 60 KB free at runtime. Every new parser uses the
  ArduinoJson filter pattern and runs on the worker task. Standings and
  weather never run while a game fetch is in flight.

## Page and app surface

- Device page (firmware/web): "When nothing is on" card, `fallback` toggle in
  Setup, "Weather location" with the geolocation button. Mock and Playwright
  tests extended for every new key.
- State document: keys above plus `fallback_mode`. The iOS app reads the
  state document; new keys are additive.
- HA: no new entities. Idle and weather are page and app settings.

## Testing

- Host tests: next-game parse (skip finals, pick first "pre"), standings
  parse, NWS forecast parse, today's finals and later-today extraction from
  the scan, precedence rule as a pure function over a fixture set.
- Playwright against the mock device: every new card and key.
- On the panel (owner): the boot log before and after, a Tuesday idle run
  through all screens, a Saturday in Live college, weather with and without a
  location, off-after-idle wake on WizMote.

## Release

v1.5.0, one changelog section in the owner voice. The v1.4.0 fallback bullet
is superseded and the changelog says so.

## Open questions for the owner

1. Today's finals rotate until midnight local. Fine, or a fixed window?
2. National Weather Service as the weather source (US only, free, no key).
3. Weather location entered on the page and app, with a browser geolocation
   button. Or ZIP code lookup instead (needs a geocoder, one more service).
