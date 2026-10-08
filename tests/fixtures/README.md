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

MLB, recorded 2026-10-07 (postseason) from the single-event endpoint
`.../baseball/mlb/scoreboard/<event id>` that per-event leagues poll:

| File | Source |
| --- | --- |
| `event_mlb_top.json` | 401908016 LAD @ ATL, Top 7th, 2-1 count, two out, runner on second |
| `event_mlb_bottom.json` | 401908016, Bot 6th, 1-1 count, one out, runner on second |
| `event_mlb_end.json` | 401908016, End 5th (stale count left in the situation) |
| `event_mlb_final.json` | 401907992 CLE @ CHW, Final 9-3, series line |
| `event_mlb_pre.json` | 401908005 MIL @ SD, pre-game with probable pitchers and odds |
| `event_mlb_hr_before.json`, `event_mlb_hr.json` | 401907987 TB @ NYY, the polls either side of a two-run home run (Top 6th) |
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

Men's college basketball, recorded 2026-10-07 between seasons from
`.../basketball/mens-college-basketball/scoreboard/<event id>`. ESPN serves a
past game only in its final state, so there is no live college fixture:

| File | Source |
| --- | --- |
| `event_mcbb_final.json` | 401856600 CONN @ MICH, 2026 national championship, Final 69-63 |
| `event_mcbb_ot.json` | 401851437 PENN @ YALE, Ivy League final, Final/OT 88-84 |
| `event_mcbb_pre.json` | 401925733 OAK @ MICH, 2026-27 opener, pre-game |
| `team_mcbb_mich.json` | `.../basketball/mens-college-basketball/teams/130` (next event is the opener) |
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

Next-game look-ahead, recorded 2026-10-08 ~14:50Z:

| File | Source |
| --- | --- |
| `team_nhl_pit_post.json` | `.../hockey/nhl/teams/16`, still naming the 10/7 final the next morning |
| `day_nhl_20261009.json` | `.../hockey/nhl/scoreboard?dates=20261009` (PIT @ CBJ among four games) |
| `day_mlb_20261009.json` | `.../baseball/mlb/scoreboard?dates=20261009` (an off day: no games) |
