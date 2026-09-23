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
| `standings_nfl_nfc_east.json` | `https://site.api.espn.com/apis/v2/sports/football/nfl/standings?group=1`, team logos and links removed (recorded 2026-09-23) |
| `standings_ncaa_acc.json` | `.../apis/v2/sports/football/college-football/standings?group=1`, first 6 entries, team logos and links removed (recorded 2026-09-23) |
| `nws_points.json` | `https://api.weather.gov/points/32.9537,-96.8903`, properties trimmed to the grid fields (recorded 2026-09-23) |
| `nws_hourly.json` | `https://api.weather.gov/gridpoints/FWD/86,112/forecast/hourly`, first 30 periods, geometry removed, pretty-printed as served (recorded 2026-09-23) |

Event ids the tests rely on are listed at the top of `test_parse.cpp`.
