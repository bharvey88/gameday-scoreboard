# PR #17 review and the new plan (handoff to the PC session)

Written 2026-09-23 by the Mac session for the session that built PR #17
(branch idle-and-startup, "Idle content, live-mode fallback and startup
(v1.5.0)"). Brandon approved everything under "Decisions" below.

## Context you may not have had

- The shipped product is **128x64** (one 128x64 matrix, or two 64x64
  chained). A lone 64x64 is supported and needs polished layouts, but it is
  not the default. Design at 128 first.
- Two PRs landed on top of main while you worked, both stacked and waiting
  on Brandon's bench check: **#14** (v1.4.4, QR code for
  https://gamedayscoreboard.app/setup on the setup, Wi-Fi lost and
  new-panel screens; rewrote firmware/pages/boot.yaml) and **#16** (v1.4.5,
  Improv Wi-Fi network scan over Bluetooth via a local
  components/esp32_improv override; also `mark_setup_done_()` so any team,
  mode or favorite pick ends the "Setup: open ..." ticker). They ship
  together as v1.4.5.
- The iOS app (PR #3 in gameday-scoreboard-ios) now: shows the panel's
  name in setup, retries a slow first join, picks the network from the
  panel's scan, only auto-opens setup with no saved panels or from the
  /setup link, and shows a "New panel nearby" strip.

## Adversarial review of #17 (summary)

Verdict: restructure before merging. Compile and 352 host checks were
green; nothing has run on a panel.

1. **High, internal RAM on a college Saturday night.** PSRAM is not used
   for malloc (`CONFIG_SPIRAM_USE_MALLOC` unset), exceptions are off. A game
   record is 104 bytes on the S3; 09-19 had 71 FBS finals, so finals grow to
   128 slots (~13.3 KB) and are copied twice (gameday.cpp:904 and :1246),
   peaking ~33 KB and holding ~27 KB against ~60 KB free.
2. **High, traffic.** Each new final card fetches a conference day
   scoreboard (184 KB ACC, 216 KB SEC measured) plus two logos every
   rotation until midnight; with every game final the early stop never
   fires, so the 2-minute scan is the full 1.2 MB (~36 MB/hour). Logo
   fetches are the suspected path in the open 2026-09-14 crash.
3. **High, design.** Idle only happens with no next event (My team) or
   nothing this week and next (live modes), which is essentially the
   off-season, so countdown/standings/record show stale data. The in-season
   Tuesday gap is untouched.
4. **High, shippability.** ~4,600 lines, no hardware run, no host tests for
   start_job_/apply_job_/decide_live_/idle_tick_/off-wake; the review-fix
   commit fixed 8 bugs in exactly that code.
5. **Medium, weather unusable for customers.** Lat/lon entry only, the
   geolocation button can't work on plain http, the app sends no location,
   a stale grid is never re-resolved (apply_idle_job_, gameday.cpp:2053).
6. **Medium, abandoned-worker race wider.** More lists in the shared job,
   `j = Job{}` resets at 1007/1984/1998/2012. Fix: per-worker heap-owned Job.
7. **Medium, extra screen at boot.** Content-ready fires on time sync, so
   boot -> clock -> scoreboard.
8. **Medium, with #16:** a no-team panel in the off-season never sees the
   setup prompt (idle pages have no ticker) and shows a Cowboys-colored clock.
9. Low: idle jobs fetch while off; standings shows overall W-L and ACC
   paging outlasts rotation; Record screen clears the opponent logo on a
   chain; postponed/canceled counted as finals; team change still shows
   "Loading" (gameday.cpp:728); ESPN week 19 returns 0 events.

Checked fine: early-stop parsing given ESPN's ordering, NWS parse and CA
chain.

## Decisions (Brandon, 2026-09-23)

Full spec: `docs/superpowers/specs/2026-09-23-playlist-and-new-panel-design.md`
(on main).

- **One playlist, one timer.** The mode only decides what goes on the list.
  Live modes: live games, then later today, then up to 6 of today's finals
  (biggest first) until midnight, then next game this week. My team: live,
  else next game alternating with a big countdown. Off-season: clock in team
  colors with a "Next:" line.
- **New panel (no team picked):** Live anything across both leagues plus a
  "Pick your team" QR card every 3rd slot; neutral colors.
- Cards drawn from the league scan's data, no per-card fetch; finals capped
  at 6 as compact records, moved not copied; with nothing live rescan at the
  earlier of next kickoff or 15 min; logos never refetched; largest-free-
  internal-block sensor; per-worker Job ownership.
- **Deferred:** standings, record, weather (until the app sends location),
  off-after-idle.

## Release plan

1. v1.4.5 = #14 + #16 (bench, then tag).
2. **v1.5.0 = startup only, ported from #17.** Plan written:
   `docs/superpowers/plans/2026-09-23-v1.5.0-startup.md` on branch
   `plan-v1.5.0` (13 tasks, ports from c672abe and 88a1b09, built on #14's
   boot.yaml; early stop and the clock-first path left for v1.5.1).
3. **v1.5.1 = the playlist + new-panel state + app changes,** reusing #17's
   parsers, fixtures and host tests. Plan to be written after v1.5.0 lands.
   Full-Saturday Live college soak on both bench panels before tagging.
4. Later: standings, record, weather, off-after-idle.

#17 stays open for reference and gets closed once v1.5.0 and v1.5.1 are
built from it. Please don't push more to #17; if you pick up work, start
from the v1.5.0 plan (after v1.4.5 merges) and follow its task order.
