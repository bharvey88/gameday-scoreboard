# HANDOFF: Game Day Scoreboard firmware, updated 2026-09-23

Companion app handoff: ~/development/gameday-scoreboard-ios/HANDOFF.md.
Audit report both repos: ~/Claude Folder/gameday-production-audit-2026-09-12.md.

## FOR THE PC SESSION (PR #17 author), read first

PR #17 was reviewed adversarially and Brandon approved a restructure: a
single playlist design, v1.5.0 = startup only (plan on branch plan-v1.5.0),
v1.5.1 = playlist + new-panel state. Everything, including the review
findings with file:line, is in
`docs/superpowers/reviews/2026-09-23-pr17-review-and-new-plan.md`.
Don't push more to #17.

## NEXT UP (from the 2026-09-23 afternoon session)

1. **Bench PR #14 (v1.4.4, setup QR code) on the second panel** gameday-74de74
   (128x64 chain, on USB at /dev/cu.usbmodem1101, joined to HarveyIoT). It
   runs v1.4.3; flash the branch with `esphome upload firmware/gameday.yaml
   --device /dev/cu.usbmodem1101` from ~/development/gameday-scoreboard-qr
   (Claude's permission classifier blocked flashing unreleased firmware;
   Brandon approves or runs it). Check: iPhone Camera scans the QR in setup
   mode, after connect, and on Wi-Fi lost, incl. low brightness; the 128
   layout (text starts ~2 px past the QR border); boot button hold shows the
   full address for 20 s. Then merge, tag v1.4.4.
2. **Failed handover disarms the 10-minute offline restart** (found by the
   #14 agent, not fixed): after a wrong password over Improv, ESPHome calls
   clear_sta(), and the restart in gameday-common.yaml bails when there is no
   network in memory. Check whether ESPHome falls back to the saved network by
   itself; if not, the panel sits offline until power-cycled.
3. **Bench PR #16 (v1.4.5, Improv Wi-Fi scan over BLE, stacked on #14).**
   Local esp32_improv override in components/esp32_improv (upstream 2026.8.2
   copy in its own commit, changes between GAMEDAY begin/end markers). Wire
   format follows improv_serial, not the published BLE spec: one RPC result
   per network [04, SSID, RSSI, YES/NO], paced 100 ms, then empty `04 00 04`
   terminator, max 20, deduped, strongest first. The published BLE spec
   packs all networks into one result (about 8 to 10 max) with auth type
   strings; settle this before any upstream PR. App side matches (iOS PR #3).
   Bench: scripts/improv_scan.py in the worktree, then provision from the app.
4. Panels on 1.4.2 or older need one USB reinstall (Brandon's gameday-2f6a70
   included).

## 2026-09-23 afternoon

- www cert: GitHub would not reissue for www (removed and re-added the
  custom domain, waited 30 min, still apex only). Fixed in Cloudflare
  instead: www CNAME is now Proxied, redirect rule "www to apex"
  (https://www.gamedayscoreboard.app/* -> https://gamedayscoreboard.app/${1},
  301). Apex records stay DNS-only so panels still see GitHub's Let's Encrypt
  cert. Side effect: applinks:www universal links won't open the app (Apple
  won't follow a redirect for AASA); nothing uses www links.
- PR #13 merged: privacy page final (draft notes gone, Wi-Fi password stays
  on the panel, ESPN/GitHub see request IPs). Published via site.yml.
- PR #12 merged: tag builds attach firmware-moonhub75.zip to the release and
  site.yml reads it (falls back to the Build artifact for v1.4.3, good until
  about 2026-12-22).
- PR #15 merged: site.yml also runs by itself on pushes to main touching
  docs/site. A gate job skips while firmware/gameday-common.yaml's version
  has no release tag yet (the tag deploy publishes docs then), which avoids
  the sha collision. Manual dispatch skips the gate: still never dispatch it
  on a commit that is about to be tagged.
- PR #14 open (v1.4.4): QR for https://gamedayscoreboard.app/setup on the
  setup, Wi-Fi lost and new-panel connected screens. 64 px: text/QR take
  turns; 128 px: QR left, static text right. Setup mode leads with "SETUP /
  Game Day / app", hotspot text only as a turn once the AP is up. No fast
  scrolling (12 px/s, per-label duration). Six review findings fixed
  (3f86bff). Needs the bench check in NEXT UP #1.
- Second panel gameday-74de74 (ESP32-S3, 16 MB flash, 8 MB octal PSRAM,
  MAC 10:20:ba:74:de:74) flashed with the v1.4.3 release factory bin after a
  full erase, then set up from the app. First Improv join timed out (see the
  iOS HANDOFF, PR #3); retry worked. Note the panel's debug log prints the
  Improv password in plain text on USB.

## 2026-09-23

- v1.4.2 shipped 2026-09-22: panels now poll gamedayscoreboard.app, and
  /setup, /privacy, /support are live. The domain is a Cloudflare zone
  (Bharvey88@gmail.com account), apex A/AAAA and www CNAME to GitHub Pages,
  all DNS-only on purpose: panels verify TLS with the ESP-IDF bundle, and
  GitHub's Let's Encrypt cert is known-good; Cloudflare proxy certs rotate CAs.
- Universal links: docs/site/.well-known/apple-app-site-association lists
  3NRQ7L5FUW.app.gamedayscoreboard.scoreboard for /setup and /setup/*.
  build.yml now copies docs/site/. (the old docs/site/* glob skipped
  dot-directories).
- .github/workflows/site.yml republishes the site without a firmware tag.
  First version ran on every docs/site push to main; that broke the v1.4.3
  deploy (Pages keys deploys by commit sha, so tagging an already-deployed
  commit is a silent no-op). Now manual dispatch only; see the later entry
  below for how it sources firmware.

## Where things stand (2026-09-22)

- main is at v1.4.1 (bc7fab8), fast-forwarded from origin. Both v1.4.0 and
  v1.4.1 have shipped since the "Where things stand" section below was
  written; that section is now history, kept for the reasoning it documents.
- 2026-09-22: PR opened from worktree ~/development/gameday-scoreboard-domain
  (branch `custom-domain`) moving the public web presence to
  gamedayscoreboard.app: firmware update source, the flasher-page link in the
  release workflow, and user-facing links in README/index.html now point
  there, and CHANGELOG bumped to v1.4.2 for it. New pages at
  docs/site/{setup,privacy,support}/index.html, linked from the index
  footer. Brandon is pointing DNS and setting the Pages custom domain
  separately; the PR says not to merge until
  https://gamedayscoreboard.app/firmware/m1/manifest.json actually resolves.
  The privacy page is a draft for his review, not legal advice.
- **v1.4.0** (panel-tells-whats-wrong, PR #9, tag v1.4.0): the panel says
  "Wi-Fi lost, open the Game Day app" instead of freezing on an old score,
  and Bluetooth switches back on so the app can hand it a new password; a
  panel that cannot get back on Wi-Fi restarts itself after 10 minutes. The
  ticker says how long since the last update when scores stop moving. A
  college team in a favorite slot now finds its games (was asking the NFL
  scoreboard). The boot screen shows a typeable address, and holding the
  boot button warns before it erases Wi-Fi (5 s grace). "Switch every" can
  go as low as 1 minute. After a final the panel moves to the next game
  instead of sitting on it. Live modes show your own team instead of a blank
  board when nothing is live. Refresh Now actually re-reads the schedule
  (was a no-op for the first six hours after a restart).
- **v1.4.1** (6389dea, bc7fab8): the panel lights up whenever it has power,
  instead of coming back dark because it happened to be off at a power cut.
  Turning it back on redraws the whole board, not just the ticker. Changing
  team, mode, a favorite button, or brightness/night from the page, app, or
  Home Assistant turns the panel on. A team logo that fails to download is
  retried a few seconds later instead of leaving a blank square.
- Host tests: 201 checks, 0 failures (`make -C tests`).
- Full detail on what shipped, the bugs subagents caught before it did, the
  bench list, and the unresolved crash below, is preserved as-is further
  down this file ("Where things stood as of 2026-09-13" section). The
  crash under "LIVE CRASH SEEN 2026-09-14" was not reproduced or explained
  as of this update; treat it as still open.

## No vendor branding (2026-09-23), branch `no-vendor-branding`

- Brandon's ask: remove every Apollo and M-1 mention, say the firmware runs
  on "an ESP32-S3 HUB75 controller on the MoonHub75 pinout" (his wording, chosen
  over "any HUB75 controller", which is untrue: the binary hardcodes that pin
  map, 16MB flash, octal PSRAM, the I2S mic on IO10/11/12 and a GPIO0 button;
  MatrixPortal S3 and Waveshare use other pins, the Trinity is not an S3).
- The pin map is the Apollo M-1 rev6 map, verified pin for pin against
  MoonModules/Hardware MOONHUB75 and projectMM's Hub75Driver.h. M-1 rev4 is
  the MatrixPortal S3 map, so rev4 boards cannot run this bin.
- Firmware: the hub75-studio controller package is no longer pulled. Its
  contents live in firmware/controllers/moonhub75.yaml with the 14 pins
  written out (no `board:` preset), included from gameday-common.yaml as the
  `controller` package. `esphome config` output diffed against main: only the
  `board:` key is gone, nothing else resolved differently.
- Docs: README (hardware section carries the 14-pin table, kept outside the
  list on purpose, a table under a `- ` item renders unreliably), installer
  page (new requirement note in the Install card, do not delete it), setup
  and support pages, CHANGELOG lines, this repo's CLAUDE.md.
- Second pass, same day: Brandon then dropped the "m1" name entirely.
  `variant: "moonhub75"` (manifest folder firmware/moonhub75/, build dir,
  CI job and artifact names), mic id `mic`, design docs de-branded too.
  No compatibility copy at firmware/m1/ ("alpha project"): panels on v1.4.2
  or older stop seeing updates and need one USB install from the site. The
  64x64/ and 128x64/ copies for pre-0.6.0 panels went at the same time.
  Version bumped to 1.4.3 with a CHANGELOG entry saying so. The only Apollo
  mention left is the rule line in CLAUDE.md that says the product is not
  an Apollo one.
- Making "any HUB75 controller" literally true is a feature, not a docs edit:
  a controller substitution, one package per board, a CI build matrix,
  per-board manifests and a picker on the installer page.

## v1.4.3 release and the Pages sha collision (2026-09-23)

- v1.4.3 tagged (eb9c89f) and released; Build run 35888705419 green. Its
  Pages deploy reported success but changed nothing: site.yml (added by the
  Mac session that day, push-triggered on docs/site) had already deployed
  the same sha at 16:19, and Pages keys deployments by GITHUB_SHA, so the
  tag's deploy at 16:32 was a silent no-op. Live site kept v1.4.2 and the
  Install button failed with "failed to download manifest".
- Setting GITHUB_SHA in a step's `env:` does NOT override it for
  deploy-pages (runner sets it afterwards; log showed the bare sha). What
  worked: a new commit on main (b281326) + `gh workflow run site.yml`.
- site.yml now: manual dispatch only, fetches firmware from the latest
  release tag's Build artifact (`firmware-moonhub75`, 90-day retention)
  instead of copying firmware/m1 from the live site. Rule: never dispatch
  it on a commit that is about to be tagged.
- Live and verified 16:38: firmware/moonhub75/manifest.json = 1.4.3,
  index says v1.4.3. Brandon installs from the local build over OTA
  (`esphome upload firmware/gameday.yaml --device gameday-2f6a70.local`) or
  from the site; either keeps settings.

## Still open, from the audit and from the sections below

- The 2026-09-14 crash (LoadProhibited fault in the Wi-Fi driver, possibly
  tied to a logo fetch) was never reproduced on a bench panel. See that
  section below for the full detail. This still outranks most other items
  if a logo fetch really can take down the Wi-Fi stack.
- The logo-URL-cache-written-before-download-resolves bug described below
  is a plausible contributor to the crash and to blank logo slots; check
  whether v1.4.1's logo retry (gd_last_team_logo guard) actually closes it.
- Everything under "Later sessions (from the audit, security removed)" below
  is still unstarted: first boot/box wording, ESPN traffic and memory
  hardening, updates/reboot timeouts, the app-firmware contract, App Store
  packaging (parked on Brandon's Apple Developer enrollment, see the iOS
  repo's HANDOFF.md), and the QR-code universal link (also parked on #6).
- Full session list and ordering: the audit report,
  ~/Claude Folder/gameday-production-audit-2026-09-12.md, and the iOS repo's
  HANDOFF.md, which tracks where Brandon is in that list.

## Where things stood as of 2026-09-13 (superseded by the above, kept for detail)

- v1.4.0 was BUILT on branch `panel-tells-whats-wrong` in worktree
  ~/development/gameday-scoreboard-tells. 23 commits, all eleven planned tasks
  plus a 6b added mid-flight. Host tests 192 checks 0 failures, and the firmware
  COMPILED (`esphome compile`, RAM 42.3%, Flash 24.3%). Nothing was flashed or
  bench tested yet at that point; both have since happened and v1.4.0 shipped.
- v1.3.8 was still on main and on Brandon's panel (gameday-2f6a70.local) at
  that point; main is now at v1.4.1.
- Plan: docs/superpowers/plans/2026-09-13-panel-tells-you-whats-wrong.md
- Spec: docs/superpowers/specs/2026-09-13-panel-tells-you-whats-wrong-design.md
  Read its "Correction, 2026-09-13, after implementation" note before trusting
  part 6: the original diagnosis was wrong.

## Verify with a compile, not a config

`esphome config` validates YAML only. It will not catch a C++ error in
components/gameday. Six tasks were "verified" with it before this was noticed.
Use:

    ~/development/tools/esphome-venv/bin/esphome compile firmware/gameday.yaml

The toolchain is warm in this worktree, so it is incremental and quick.

## What actually shipped, versus the spec

Three places where the code deliberately differs from the approved spec. Each
was found by reading the source, and each is in the plan with reasoning.

1. **Part 6's diagnosis was wrong.** `start_job_`:885 already forced a schedule
   re-read 30 minutes after a game goes POST. The panel in the field report was
   re-reading all night and getting the same finished event back, because ESPN's
   `team.nextEvent[0]` was itself stuck. The apply-side block the spec asked for
   was also unreachable: `post_since_ms_` and `schedule_fetched_ms_` are stamped
   from the same `now` in the same call, so its throttle could never be met.
   That block was written, proven dead by simulation, and deleted. What shipped
   is Task 6b: take the next event from `upcoming_` (the season schedule, which
   lists only games that have not started) inside the existing post-linger block.
   The panel still moves at final + 30 minutes. What changed is what it moves to.
2. **Part 6's "emit on every poll" was already the behaviour.** Dropped.
3. **Part 2 needed no device-page change.** The page already renders `status`
   straight into the ticker, so the firmware's new wording arrives for free.

## Bugs the subagents caught in the plan, worth knowing

These were all in the written plan and would have shipped:

- `schedule_fetched_ms_ = 0` does not force a schedule re-read while `schedule_`
  is still valid. Clearing `schedule_` is what does. The same idiom is still in
  `refresh_now()` and is filed as its own task.
- `has_sta()` is TRUE after the Reset Wi-Fi button, because it saves a blank
  SSID. The ten minute reboot would have rebooted every freshly reset panel
  every ten minutes while it sat in setup. Use
  `has_sta() && get_sta().get_ssid().empty()`, the idiom already at
  gameday-common.yaml:66.
- `our_id_()` and `decide_splash`'s `neutral` flag both key off `live_mode_()`.
  In the live fallback the board shows the owner's own team, so both needed
  `&& !live_fallback_` or the card would be oriented wrong and celebrate like a
  stranger's game.
- The My team path ends with `schedule_next_(interval_for_phase_())`, which would
  have parked the fallback for 15 minutes or 6 hours and swallowed the 2 minute
  rescan. Capped in `interval_for_phase_()` under `live_fallback_`.
- `component.suspend` has no matching `suspend()` method. From a lambda it is
  `stop_poller()` / `start_poller()` on a PollingComponent.
- `improv.is_active()` and `captive_portal.is_active()` are both effectively
  sticky-true for a whole outage. Neither works as a "busy" guard. The real
  mid-handover state is `improv::STATE_PROVISIONING`.

## Timings that matter on the bench

- Wi-Fi drops: overlay immediately. BLE comes back at 30s (a `mode: restart`
  grace script, cancelled if Wi-Fi returns). Fallback AP at 60s, and the screen
  switches to the proven hotspot text there. Improv advertises at 90s, gated by
  its own `wifi_timeout`, so the app cannot see the panel before then. Brandon
  chose to keep "open the Game Day app" from the start regardless.
- ESPHome's own `reboot_timeout` is dead while an `ap:` block exists, which is
  why the ten minute reboot is hand rolled.
- Ten minutes is tight: about 8.5 usable minutes to notice the panel, fetch a
  phone and provision. The guard only covers the credential handover itself.
  Fifteen was suggested and declined for now; it is a one character change.

## LIVE CRASH SEEN 2026-09-14, v1.3.8, not yet explained

Brandon's panel (gameday-2f6a70.local, running shipped v1.3.8) crashed and
rebooted while it was being watched. Captured from `esphome logs`:

    *** CRASH DETECTED ON PREVIOUS BOOT ***
      Reason: Fault - LoadProhibited (cause 28)
      Crashed core: 0
      PC:  0x40055EBF   EXCVADDR: 0x00000008
      BT0: 0x40386CDF  lmacEndFrameExchangeSequence
      BT1: 0x421736DD  generic_error_category::~generic_error_category()
      BT2: 0x421737B5  get_iq_value
      BT3: 0x4038665D  ppTask

Null dereference (faulting address 0x8) on core 0, backtrace inside the ESP32
Wi-Fi driver: `lmac` is the low level MAC and `ppTask` is the packet processor.

Context, all circumstantial:

- The panel had just had its team changed twice over the HTTP API, deliberately,
  to force a team logo re-fetch after the logos failed to appear.
- The only unusual network activity in that window was the `online_image`
  logo download: an HTTPS fetch to a.espncdn.com plus a PNG decode, a different
  host from the ESPN API the panel normally talks to.
- Both logo URLs were verified reachable from a laptop at the time (HTTP 200,
  1709 and 2970 byte PNGs through the combiner).
- The panel recovered on its own and polled normally afterwards: misses 0,
  PSRAM 6.1 MB free. It was not seen to crash twice.
- Not reproduced. One occurrence.

Why this matters more than it looks: if a logo fetch can take down the Wi-Fi
stack, that outranks most of the remaining audit list, and every logo-related
item below is really a symptom. It also means the "logo fetch retry on failure"
item is not a small UX fix.

Next step is a reproduction, not a patch. Change the team a number of times with
`esphome logs` attached and see whether the crash follows the logo download. Do
that on a bench panel, not on the one in the living room.

Related, and now confirmed worse than the audit described: the logo URL cache
(`gd_last_team_logo` / `gd_last_opp_logo`, gameday-common.yaml around line 360)
is written BEFORE the download resolves. The guard is
`!x.team_logo.empty() && x.team_logo != id(gd_last_team_logo)`, so a single
failed download means that logo never retries until a reboot or another team
change. On Brandon's panel the result was a blank image slot rendering as a
single stray glyph.

## Reading the panel's log on this Mac

`timeout` does not exist here (no coreutils, no Homebrew). A command built
around it fails silently and produces an empty result that reads like "nothing
happened". Use:

    sh -c '~/development/tools/esphome-venv/bin/esphome logs firmware/gameday.yaml \
      --device gameday-2f6a70.local > /tmp/gdlog.txt 2>&1 & p=$!; sleep 30; kill $p'

## Panel HTTP API, verified working 2026-09-14

- `POST /gameday/set?team=nfl%3A7` needs a Content-Length even though the params
  are in the query string. Bare `curl -X POST` gets 411. Use `-d ''`.
- The switch route uses the entity NAME, not the YAML id:
  `POST /switch/Power/turn_on` works, `/switch/power/turn_on` is a 404.
- `GET /gameday/state` is the quickest read of team, score, misses and version.

## Bench list for Brandon

From the spec, plus what the implementation turned up:

- (a) change the router password, panel says Wi-Fi lost within 60s, app finds it
  over Bluetooth AFTER about 90s, joins, normal screen returns
- (b) reboot the router only, panel recovers on its own with no reboot
- (c) block ESPN during a game, ticker says "no update for N min" within 3 polls
- (d) leave Wi-Fi off 11 min, panel reboots, and does NOT reboot every minute
- (e) Alabama in slot 1 shows a game
- (f) hold the button 2s, 6s and 11s and see each stage; also release at 7s and
  confirm nothing is reset
- (g) **the important one**: Free Heap (internal), now on by default, across
  several forced Wi-Fi outages longer than 30s. Does internal heap return to its
  pre-drop level after reconnect and `ble.disable`, over repeated cycles? This is
  the v1.3.4 fragmentation question and the compile cannot answer it.
- (h) after a final, panel moves to the next game at +30 min, logging "Game is
  over: taking event <id> from the season schedule". If it logs "Fetching
  schedule:" instead, `upcoming_` was empty and it took the fallback.
- (i) Live college on an NFL Sunday shows the saved team's card with
  "No live college games, showing DAL" in the ticker
- (j) leave it in the fallback 10 minutes: the card must keep updating and the
  panel must not go quiet for 15 minutes
- (k) from the fallback, a kickoff takes the board back within 2 minutes, with no
  splash on the first frame
- Check `GET /gameday/state` for `stale_s`, `fallback`, and `game.l` matching the
  saved team's league while the fallback is up

## Known limitations, accepted for this release

- An abandoned worker cannot signal `job_done_` (`worker_seq_`) but can still
  write into the shared `this->job_`. Real fix is per-worker Job ownership.
- The ten minute reboot can fire while someone is typing into the captive portal.
  No upstream signal to guard on.
- A hold through the 5s warning during first boot leaves `boot_check` stopped for
  that boot. Improv's `on_start` and `on_provisioned` recover it in the app flow.
- `current_team_()` defaults to Dallas, so a panel that never picked a team still
  falls back to DAL rather than an empty board.

## Filed separately, not in this release

- `scripts/build_web.py` cannot run under its own shebang: it needs 3.10+ for
  `Path.write_text(newline=)` and this Mac's python3 is 3.9.6. Use
  ~/development/tools/esphome-venv/bin/python.
- `refresh_now()` does not force a schedule re-read for the first six hours of
  uptime, same broken idiom as above.

## Next session: the app (gameday-scoreboard-ios)

- Wrong Wi-Fi password shows an error (ImprovClient.swift:239, SetupWizardView.swift:157).
- 2.4 GHz note under the password field; failure text lists the real causes.
- 20 s connect timeout with Rescan; Bluetooth off mid-flow reported.
- Only devices named `gameday-*` in the setup list and the waiting screen.
- Offline card: stream timeout 35 s, after ~20 s unreachable dim the card and say
  "Not connected to the panel. Scores may be out of date." (PanelHomeView.swift:32,
  PanelModel.swift:162, DeviceClient.swift:75).
- Stale badge reads `stale_s` (Game.swift:73 reads `game.m`, which the firmware never sends).
- Mode row shows the fallback note from spec part 7.
- Steppers 1...30 (ModeBarView.swift:56, 75).
- Permission prompts: create the Bluetooth manager lazily, explain both permissions
  on the Panels screen with a Settings button (AppModel.swift:8, ImprovClient.swift:52,
  PanelsView.swift:46).
- Add by address normalises the typed name (.local, no scheme) (PanelsView.swift:136).
- Setup wizard fallback must not replace the .local entry with the Improv URL
  (SetupWizardView.swift:184).
- Failed writes revert the control (PanelModel.swift:186, 269, 278, 288).
- Reboot wait cancellable, gives up at ~90 s with a message, task stored and
  cancelled in stop() (PanelModel.swift:403).

## Later sessions (from the audit, security removed)

- First boot and the box: README and installer page rewritten around the app flow,
  Bluetooth setup message not overwritten by hotspot text (boot.yaml:215),
  unique hotspot name, installer wording for Mac Safari.
- ESPN traffic and memory: 15 s live poll with a locally ticking clock, backoff
  with jitter, 429/403 as long backoffs, JSON documents in PSRAM, live modes scan
  the previous day after midnight Eastern, postponed/canceled games shown as such
  and no WINS splash without `completed`.
- Updates and reboots: device page install/reboot waits get timeouts and messages,
  page fetches get abort timeouts, hardware pass on Reboot, Install, Unpair, panel
  count, RELEASING.md, tag/YAML/CHANGELOG consistency check in CI.
- Contract: timezone by name, 400 from an old firmware explained not retried,
  mock matches firmware clamps and types, CI for the app, bundle.js freshness check.
- Logo fetch retry on failure (gameday-common.yaml:283, 297), logo timeout,
  WizMote unpair persisted (wizmote.yaml:182).
- App Store package: parked until Brandon reopens #6.
- QR code to get the app: parked with #6 (Brandon, 2026-09-13). Researched, not
  built. A single https Universal Link does both halves: iOS opens the app when
  it is installed, otherwise the page loads and sends them to the App Store.
  Needs all three of #6: an Apple Developer account (Associated Domains wants a
  Team ID), a domain serving /.well-known/apple-app-site-association, and a
  bundle id. Firmware side is not the blocker: ESPHome 2026.8.2 has the
  `qr_code` component, and a version 3 code (29x29 modules) at scale 2 is 58 px,
  which fits a 64x64 panel with a quiet zone. The URL is the stable part, so
  picking it once means the behaviour upgrades with the AASA file and the panel
  never needs reflashing. Two things to bench when it is unparked: render the
  light modules and quiet zone lit and the dark modules off (a QR wants dark on
  light, and white-on-black is inverted), and confirm the iOS Camera app honours
  a universal link from a QR scan. Placement was never settled: the panel covers
  the "Wi-Fi lost, open the Game Day app" screen from spec part 1, where the
  customer does not know which app, but the box or a sticker may matter more at
  unboxing.

## Open question

Brandon reported "an old game with outdated details" while the state document said
NOT_FOUND in Live college mode. The display code should have shown the no-game
layout. A photo of the panel in that state would tell whether the screen can lag
the state document (would be a real display bug, not yet seen in code).

## How to work here

- Every agent in its own worktree: `git worktree add ../gameday-scoreboard-<task> -b <branch> main`.
- Commit as `bharvey88 <8107750+bharvey88@users.noreply.github.com>`, no Claude credit,
  multi-line messages via `git commit -F`.
- Bump `version` in firmware/gameday-common.yaml and add a CHANGELOG section for
  every release; a tag without a section fails CI.
- ESPHome venv: ~/development/tools/esphome-venv (2026.8.2). `esphome logs
  firmware/gameday.yaml --device gameday-2f6a70.local` streams the panel's log.
  USB flash at /dev/cu.usbmodem1101. Brandon merges, tags and flashes.
- Host tests: `make -C tests` (192 checks as of v1.4.0). Rebuild the page bundle
  with `~/development/tools/esphome-venv/bin/python scripts/build_web.py` after
  editing firmware/web/app.js. Bare python3 on this Mac is 3.9.6 and the script
  needs 3.10+ (Path.write_text newline=), so it fails with its own shebang.
- Subagents: Sonnet or Opus for well-specified work, Fable only for hard judgment.
  Read-only on the panel unless Brandon asks (GET only, no POSTs).
