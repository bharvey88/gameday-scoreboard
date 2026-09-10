# Favorite Teams mode (v1.2.0)

Fifth mode, "Favorite teams". The list is the four Favorites slots (the
WizMote buttons) in slot order; slot order is the priority.

## What the panel does

- Idle: no favorite is near a game. The panel cycles each favorite's
  next-game card, 10 seconds each, in slot order. Only the 6-hour schedule
  refresh runs; nothing is polled.
- Lock-on: a favorite whose game is in progress, or kicks off within the
  lock-on window (default 15 minutes, 5 to 120), takes the panel and is polled
  at the normal cadence. After the final it holds for the release time
  (default 30 s, 30 s to 60 min), then the playlist resumes and that team's
  next game is fetched.
- Collision: two or more locked. "Stick with the higher team" (default) shows
  the lowest slot. "Alternate" flips between the locked games on the existing
  rotate timer.
- Remote: a button press jumps to that favorite and pins it. Locked, it holds
  regardless of priority until it releases; idle, it seeds the playlist.
- No favorites set: acts like My team with a ticker prefix pointing at the page.

## Implementation

- `components/gameday/favorites.h`: pure decision logic (`fav_locked`,
  `pick_favorite`) on epoch seconds, covered by `tests/test_parse.cpp`.
- `GamedayComponent`: `FavEntry` per set slot (team, schedule, snapshot,
  final time). The worker refreshes one entry per cycle or polls the shown
  game; the main loop ticks every second and applies `pick_favorite`.
- Prefs4 (`gameday_prefs4_v1`): lockon_minutes, release_seconds, collide.
- `/gameday/set`: `lockon`, `release`, `collide`. State document: `lockon`,
  `release`, `collide`, `shown` (slot), `locked`, and in this mode `next` is
  one row per favorite (`slot`, `t`, `s`, `kick`, `oi`, `oa`, `ts`, `os`, `tv`,
  `detail`).
- Page: mode pill 4, a settings bar in place of the team bar, up/down arrows
  on the Favorites card (a move posts all four slots), Up next lists every
  favorite.
