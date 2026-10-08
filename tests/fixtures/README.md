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

Soccer, from the single-event endpoint `.../soccer/<league>/scoreboard/<event id>`.
Brazilian Serie A (`bra.1`, the same document shape as MLS and the Premier
League) recorded live 2026-10-08; the rest recorded 2026-10-08 from past and
upcoming games:

| File | Source |
| --- | --- |
| `event_soccer_first_half.json` | 401841249 SAO @ CRU, 25', 0-0, a yellow card each |
| `event_soccer_halftime.json` | 401841257 VAS @ BOT, HT, VAS 1-0 |
| `event_soccer_second_half.json` | 401841253 MIR @ BRA, 90', 1-1, MIR red card at 28' |
| `event_soccer_stoppage.json` | 401841255 COR @ INT, 90'+2', INT 2-1, status name `STATUS_IN_PROGRESS` |
| `event_soccer_full_time.json` | 401841255, FT, INT win 2-1 |
| `event_epl_pre.json` | `soccer/eng.1` 401879268 LEE @ ARS, pre-game with the three-way moneyline |
| `event_mls_pre.json` | `soccer/usa.1` 761847 DC @ MIA, pre-game |
| `event_epl_final.json` | 401878779 ARS @ SUN, FT 0-2, SUN red card, ARS penalty at 90'+7' |
| `event_mls_final.json` | 761829 SD @ MIA, FT 2-2 (a draw) |
| `event_soccer_pens.json` | `soccer/uefa.champions` 401862897 ARS @ PSG, 2026 final, 1-1, PSG win 4-3 on penalties |
| `event_soccer_aet.json` | `soccer/fifa.world` 760500 CPV @ ARG, AET 3-2, own goal at 111' |
| `team_epl_ars.json` | `.../soccer/eng.1/teams/359` |
| `team_mls_mia.json` | `.../soccer/usa.1/teams/20232` |
| `team_ucl_ars.json` | `.../soccer/uefa.champions/teams/359`: the same club's next Champions League game |
| `team_uel_ars.json` | `.../soccer/uefa.europa/teams/359`: a club not in the competition gets HTTP 200 with no `nextEvent` |
| `schedule_mls_mia.json` | `.../soccer/usa.1/teams/20232/schedule?fixture=true`, trimmed to the first 2 events (without `fixture=true` the endpoint lists past results) |

`football_golden.txt` is written by the host tests (`GOLDEN_WRITE=1`) and
pins football's output; see `golden_football.cpp`.
