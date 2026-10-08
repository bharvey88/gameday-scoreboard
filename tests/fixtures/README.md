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

Event ids the tests rely on are listed at the top of `test_parse.cpp`.

MLB, recorded 2026-10-07 (postseason) from the single-event endpoint
`.../baseball/mlb/scoreboard/<event id>` that per-event leagues poll:

| File | Source |
| --- | --- |
| `event_mlb_top.json` | 401908016 LAD @ ATL, Top 7th, 2-1 count, two out, runner on second |
| `event_mlb_bottom.json` | 401908016, Bot 6th, 1-1 count, one out, runner on second |
| `event_mlb_end.json` | 401908016, End 5th (stale count left in the situation) |
| `event_mlb_final.json` | 401907992 CLE @ CHW, Final 9-3, series line |
| `event_mlb_pre.json` | 401908005 MIL @ SD, pre-game with probable pitchers and odds |
| `team_mlb_atl.json` | `.../baseball/mlb/teams/15` (next event is the game in progress) |

`football_golden.txt` is written by the host tests (`GOLDEN_WRITE=1`) and
pins football's output; see `golden_football.cpp`.
