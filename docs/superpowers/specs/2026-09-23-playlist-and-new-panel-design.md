# One playlist for what the panel shows, plus the new-panel state

Date: 2026-09-23. Approved by Brandon in the Mac session the same day.
Replaces the live-mode and idle parts of PR #17 (spec
2026-09-23-idle-and-startup-design.md), after an adversarial review of #17
found internal-RAM, traffic and scope problems. #17's parsers, fixtures and
host tests are reused; #17 stays open for reference and is closed once
v1.5.0 and v1.5.1 are built from it.

## Goals

- A panel is never blank and never shows "No upcoming game" while it is
  only looking.
- A brand-new panel (no team picked yet) shows real games right away and
  keeps reminding the owner to pick a team, without nagging.
- Less code, less internal RAM and fewer downloads than #17, not more than
  v1.4.3 on a busy college Saturday.

Out of scope for now: standings, season record, weather, off-after-idle.
They come back later, weather only once the app can send a location.

## Hardware

128x64 is the default layout (one 128x64 matrix, or two 64x64 chained).
A lone 64x64 is supported and gets polished layouts, but is not the default.
Every card below has both a 128 and a 64 layout.

## The playlist

The panel steps through one ordered list of cards, one card per
"Switch every" period, with one timer. The mode only changes what goes on
the list:

| Mode | The list, in order |
|---|---|
| My team | The team's live game. Otherwise its next game, alternating with a big countdown card. After a final: the final until local midnight, then next game and countdown. |
| Live NFL / Live college / Live anything | Live games in that league. Otherwise: games later today, then up to 6 of today's finals (biggest first) until local midnight, then the next game this week. |
| Favorites | The same rules, over the favorite teams. |
| No team picked yet | Live anything across both leagues, plus the "Pick your team" card every 3rd slot. |
| Nothing scheduled (off-season) | Clock in team colors, with "Next: DAL vs PHI Sun 3:25" when a next game is known. |

"No team picked yet" is the existing setup-done flag (v1.4.5, set by any
team, mode or favorite pick from any client). One helper decides the
effective mode, so no-team is not a separate chooser.

"Pick your team" card: QR code for https://gamedayscoreboard.app/setup on
the left and "Pick your / team in / the app" on the right at 128; text and
QR take turns at 64, like the setup screen. The clock card on a no-team
panel uses neutral colors, not the default team's.

"Biggest first" for finals: ranked or primetime games first, then the rest
in kickoff order. Postponed and canceled games are not finals and never
appear.

## Data and resources

- Cards are drawn from the league scan the panel already makes (score,
  logos, records, color, detail, TV). Only a live game gets its own poll.
  No per-card scoreboard fetch.
- Finals are capped at 6 and stored as compact records. Lists are moved,
  never copied, and the job's lists are cleared after they are applied.
- With nothing live, the next scan is at the earlier of the next kickoff or
  15 minutes (v1.4.3 and #17 scan every 2 minutes).
- A logo already on the panel is never downloaded again for a card.
- New diagnostic: largest free internal RAM block, as a sensor.
- Per-task job ownership (each fetch task writes only into its own
  heap-allocated job), closing the abandoned-worker race HANDOFF lists.

## The app

- After Wi-Fi setup the app goes straight to "Pick your team", with Skip.
- If skipped, the panel's home screen shows a "Pick a team" strip until a
  team or mode is picked.
- When exactly one panel is saved (the app opens straight to it), a "New
  panel nearby, tap to set up" strip shows on that screen while a new panel
  is advertising for setup. Approved by Brandon; built in iOS PR #3.

## Releases

1. v1.4.5: PRs #14 and #16 (QR screens, Wi-Fi network list). Bench, then tag.
2. v1.5.0, startup only, taken from #17: 500 ms readiness poll, boot screen
   held until the first scoreboard with a 10 s cap, no "----" or
   "Loading" board (including on team change), team info cached between
   boots. Before and after USB boot logs measured on a panel.
3. v1.5.1: the playlist, the new-panel state and the app changes above.
4. Later: standings, record, weather, off-after-idle.

## Testing

- Host tests for the playlist builder: every row of the mode table,
  midnight rollover in US time zones, the finals cap and ordering,
  postponed and canceled games, the 3rd-slot reminder, rescan timing.
- `esphome compile`, `make -C tests`, an independent review pass, then
  Brandon's hardware check before any tag.
- Before tagging v1.5.1: both bench panels in Live college for a full
  Saturday, watching the largest-free-block sensor and misses.
