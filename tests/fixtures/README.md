# Test fixtures

Recorded from ESPN on 2026-09-05 with a `curl/8.0` User-Agent (ESPN returns
403 to most other agents).

| File | Source |
| --- | --- |
| `team_nfl_dal.json` | `.../football/nfl/teams/dal` |
| `team_ncaa_bc.json` | `.../football/college-football/teams/103` |
| `scoreboard_nfl.json` | `.../football/nfl/scoreboard` (week 1, all pre) |
| `schedule_nfl_dal.json` | `.../football/nfl/teams/6/schedule`, trimmed to the first 5 events (recorded 2026-09-08) |
| `schedule_ncaa_bc.json` | `.../football/college-football/teams/103/schedule`, trimmed to the first 5 events (recorded 2026-09-08; the first is already final) |
| `scoreboard_ncaa_live.json` | `.../football/college-football/scoreboard?groups=80&limit=200` (mix of pre, in, post) |
| `scoreboard_ncaa_conf_titles.json` | `.../college-football/scoreboard?groups=80&limit=300&dates=20251206`, conference title games, all final (recorded 2026-10-08) |
| `scoreboard_ncaa_cfp_quarterfinals.json` | `.../college-football/scoreboard?groups=80&limit=300&dates=20260101`, CFP quarterfinals at neutral sites (recorded 2026-10-08) |
| `scoreboard_ncaa_cfp_final.json` | `.../college-football/scoreboard?groups=80&limit=300&dates=20260119`, CFP national championship (recorded 2026-10-08) |
| `scoreboard_nfl_wild_card.json` | `.../football/nfl/scoreboard?dates=20260110`, two wild card games (recorded 2026-10-08) |
| `scoreboard_nfl_super_bowl.json` | `.../football/nfl/scoreboard?dates=20260208`, Super Bowl LX (recorded 2026-10-08) |

Event ids the tests rely on are listed at the top of `test_parse.cpp`.
