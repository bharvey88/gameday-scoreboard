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

NHL, recorded 2026-10-07 and 2026-10-08 UTC from the single-event endpoint
`.../hockey/nhl/scoreboard/<event id>`:

| File | Source |
| --- | --- |
| `event_nhl_live.json` | 401892455 COL @ WPG, 3:21 left in the 1st, 0-0, a delay of game penalty as the last play |
| `event_nhl_end.json` | 401891830 PIT @ WSH, End of 1st (status END_PERIOD, the intermission), 2-1 WSH |
| `event_nhl_final.json` | 401891830 PIT @ WSH, Final 5-3 WSH |
| `event_nhl_pre.json` | 401892456 EDM @ ANA, pre-game with odds and O/U |
| `event_nhl_final_so.json` | 401803369 PIT @ CAR, 2026-03-10, Final/SO 5-4 CAR (recorded after the fact) |
| `event_nhl_final_2ot.json` | 401869781 OTT @ CAR, 2026 playoffs, Final/2OT 3-2 CAR, series line (recorded after the fact) |
| `team_nhl_edm.json` | `.../hockey/nhl/teams/6` (next event is the EDM @ ANA game above) |

`football_golden.txt` is written by the host tests (`GOLDEN_WRITE=1`) and
pins football's output; see `golden_football.cpp`.
