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

NBA (preseason) and WNBA (semifinals), recorded 2026-10-07 from
`.../basketball/{nba,wnba}/scoreboard/<event id>`:

| File | Source |
| --- | --- |
| `event_nba_live.json` | 401914123 MIN @ IND, 8:27 3rd, 74-73 |
| `event_nba_final.json` | 401914123, Final 123-112 |
| `event_nba_clock.json` | 401908620 MIL @ OKC, 12.3 1st (tenths, no minutes) |
| `event_nba_end.json` | 401908620, End of 1st |
| `event_nba_pre.json` | 401914129 GS @ POR, pre-game with odds |
| `event_wnba_live.json` | 401918297 NY @ ATL, 4.5 2nd, ATL leads series 1-0 |
| `event_wnba_half.json` | 401918297, Halftime |
| `event_wnba_ot.json` | 401918297, 20.8 OT |
| `event_wnba_final.json` | 401918297, Final/OT 101-98 (series line not yet updated) |
| `team_nba_ind.json` | `.../basketball/nba/teams/11` (next event is the game in progress) |
| `team_wnba_ny.json` | `.../basketball/wnba/teams/9` (next event is the game in progress) |

`football_golden.txt` is written by the host tests (`GOLDEN_WRITE=1`) and
pins football's output; see `golden_football.cpp`.
