// Host tests for the ESPN parser and game logic. Build with `make` in tests/.
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "espn_parse.h"
#include "favorites.h"
#include "idle.h"
#include "weather.h"

using namespace espn;

static int failures = 0;
static int checks = 0;

#define CHECK(cond)                                                            \
  do {                                                                         \
    checks++;                                                                  \
    if (!(cond)) {                                                             \
      failures++;                                                              \
      printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                   \
    }                                                                          \
  } while (0)

#define CHECK_EQ(a, b)                                                         \
  do {                                                                         \
    checks++;                                                                  \
    auto va = (a);                                                             \
    auto vb = (b);                                                             \
    if (!(va == vb)) {                                                         \
      failures++;                                                              \
      std::ostringstream os;                                                   \
      os << va << " != " << vb;                                                \
      printf("FAIL %s:%d: %s: %s\n", __FILE__, __LINE__, #a, os.str().c_str()); \
    }                                                                          \
  } while (0)

static std::string slurp(const char *path) {
  std::ifstream f(path, std::ios::binary);
  std::stringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

// Fixture facts (recorded 2026-09-05, see fixtures/README.md)
static const char *kLiveEvent = "401858433";  // BOIS @ ORE, 15:00 4th, ORE ball
static const char *kPostEvent = "401858432";  // BALL @ OSU, Final 3-56
static const char *kNflEvent = "401872656";   // SEA vs NE, 9/9 8:20 PM EDT
static const uint32_t kBoise = 68, kOregon = 2483, kBallState = 2050, kOhioState = 194, kSeahawks = 26, kPatriots = 17;

static void test_urls() {
  CHECK_EQ(team_url(League::NFL, 6), std::string("https://site.api.espn.com/apis/site/v2/sports/football/nfl/teams/6"));
  // Monday night kickoff 00:20Z on 9/15 is still 9/14 in the US
  CHECK_EQ(scoreboard_url(League::NFL, 0, parse_iso8601_z("2026-09-15T00:20Z")), std::string("https://site.api.espn.com/apis/site/v2/sports/football/nfl/scoreboard?dates=20260914"));
  // 2026-09-05T19:30Z is Sep 5 in the US; 2026-09-06T02:00Z is still Sep 5 Eastern
  CHECK_EQ(scoreboard_url(League::NCAA, 1, parse_iso8601_z("2026-09-05T19:30Z")),
           std::string("https://site.api.espn.com/apis/site/v2/sports/football/college-football/scoreboard?groups=1&dates=20260905"));
  CHECK_EQ(scoreboard_url(League::NCAA, 9, parse_iso8601_z("2026-09-06T02:00Z")),
           std::string("https://site.api.espn.com/apis/site/v2/sports/football/college-football/scoreboard?groups=9&dates=20260905"));
  CHECK_EQ(dark_logo("https://a.espncdn.com/i/teamlogos/ncaa/500/68.png"), std::string("https://a.espncdn.com/combiner/i?img=/i/teamlogos/ncaa/500-dark/68.png&w=64&h=64"));
  CHECK_EQ(dark_logo("https://a.espncdn.com/i/teamlogos/nfl/500/scoreboard/sea.png"), std::string("https://a.espncdn.com/combiner/i?img=/i/teamlogos/nfl/500-dark/scoreboard/sea.png&w=64&h=64"));
  CHECK_EQ(team_logo_url(League::NFL, 6, "DAL"), std::string("https://a.espncdn.com/combiner/i?img=/i/teamlogos/nfl/500-dark/dal.png&w=64&h=64"));
  CHECK_EQ(team_logo_url(League::NCAA, 228, "CLEM"), std::string("https://a.espncdn.com/combiner/i?img=/i/teamlogos/ncaa/500-dark/228.png&w=64&h=64"));
  CHECK_EQ(dark_logo(""), std::string(""));
}

static void test_iso() {
  CHECK_EQ(parse_iso8601_z("1970-01-01T00:00Z"), (int64_t) 0);
  CHECK_EQ(parse_iso8601_z("2026-09-05T19:30Z"), (int64_t) 1788636600);
  CHECK_EQ(parse_iso8601_z("2026-09-10T00:20:00Z"), (int64_t) 1788999600);
  CHECK_EQ(parse_iso8601_z("garbage"), (int64_t) 0);
}

static void test_team_parse() {
  Schedule s;
  CHECK(parse_team_str(slurp("fixtures/team_ncaa_bc.json"), s));
  CHECK(s.valid);
  CHECK_EQ(s.event_id, std::string("401856777"));
  CHECK_EQ(s.kickoff_epoch, parse_iso8601_z("2026-09-05T19:30Z"));
  CHECK_EQ(s.group, (uint32_t) 1);
  CHECK_EQ(s.team_color, std::string("8c2232"));
  CHECK_EQ(s.team_record, std::string("0-0"));

  Schedule d;
  CHECK(parse_team_str(slurp("fixtures/team_nfl_dal.json"), d));
  CHECK_EQ(d.event_id, std::string("401872930"));
  CHECK_EQ(d.team_color, std::string("002a5c"));
  CHECK_EQ(d.team_record, std::string("2-1"));

  Schedule bad;
  CHECK(!parse_team_str("{not json", bad));
  CHECK(!bad.valid);
}

static void test_scoreboard_live() {
  std::string json = slurp("fixtures/scoreboard_ncaa_live.json");
  GameSnapshot s;
  CHECK(parse_scoreboard_str(json, kLiveEvent, kBoise, s));
  CHECK(s.valid);
  CHECK(s.state == GameState::IN);
  CHECK_EQ(s.team_abbr, std::string("BOIS"));
  CHECK_EQ(s.opp_abbr, std::string("ORE"));
  CHECK_EQ(s.team_id, kBoise);
  CHECK_EQ(s.opp_id, kOregon);
  CHECK_EQ(s.team_score, 24);
  CHECK_EQ(s.opp_score, 24);
  CHECK_EQ(s.possession, 2);  // Oregon has the ball
  CHECK_EQ(s.team_timeouts, 1);
  CHECK_EQ(s.opp_timeouts, 1);
  CHECK_EQ(s.down_distance, std::string("3rd & 10 at BOIS 25"));
  CHECK_EQ(s.short_down_distance, std::string("3rd & 10"));
  CHECK(!s.is_red_zone);
  CHECK_EQ(s.short_detail, std::string("15:00 - 4th"));
  CHECK_EQ(s.period, 4);
  CHECK_EQ(s.team_color, std::string("0033a0"));
  CHECK_EQ(s.opp_color, std::string("00934b"));
  CHECK_EQ(s.team_logo, std::string("https://a.espncdn.com/combiner/i?img=/i/teamlogos/ncaa/500-dark/68.png&w=64&h=64"));
  CHECK_EQ(s.opp_logo, std::string("https://a.espncdn.com/combiner/i?img=/i/teamlogos/ncaa/500-dark/2483.png&w=64&h=64"));
  CHECK(s.last_play.rfind("End of 3rd quarter", 0) == 0);
  CHECK(!s.completed);

  // Same event from Oregon's side flips everything
  GameSnapshot o;
  CHECK(parse_scoreboard_str(json, kLiveEvent, kOregon, o));
  CHECK_EQ(o.team_abbr, std::string("ORE"));
  CHECK_EQ(o.possession, 1);

  // Event present but we are not in it
  GameSnapshot x;
  CHECK(!parse_scoreboard_str(json, kLiveEvent, kSeahawks, x));
  // Event missing
  CHECK(!parse_scoreboard_str(json, "1", kBoise, x));
}

static void test_scoreboard_post() {
  std::string json = slurp("fixtures/scoreboard_ncaa_live.json");
  GameSnapshot s;
  CHECK(parse_scoreboard_str(json, kPostEvent, kBallState, s));
  CHECK(s.state == GameState::POST);
  CHECK(s.completed);
  CHECK_EQ(s.team_score, 3);
  CHECK_EQ(s.opp_score, 56);
  CHECK(!s.team_winner);
  CHECK_EQ(s.team_record, std::string("0-1"));
  CHECK_EQ(s.opp_record, std::string("1-0"));
  CHECK_EQ(s.short_detail, std::string("Final"));
  GameSnapshot w;
  CHECK(parse_scoreboard_str(json, kPostEvent, kOhioState, w));
  CHECK(w.team_winner);
}

static void test_scoreboard_pre_nfl() {
  std::string json = slurp("fixtures/scoreboard_nfl.json");
  GameSnapshot s;
  CHECK(parse_scoreboard_str(json, kNflEvent, kPatriots, s));
  CHECK(s.state == GameState::PRE);
  CHECK_EQ(s.team_abbr, std::string("NE"));
  CHECK_EQ(s.opp_abbr, std::string("SEA"));
  CHECK_EQ(s.odds, std::string("SEA -3.5"));
  CHECK_EQ(s.over_under, std::string("44.5"));
  CHECK_EQ(s.tv, std::string("NBC"));
  CHECK_EQ(s.venue, std::string("Lumen Field"));
  CHECK_EQ(s.kickoff_epoch, parse_iso8601_z("2026-09-10T00:20Z"));
  CHECK_EQ(s.team_logo, std::string("https://a.espncdn.com/combiner/i?img=/i/teamlogos/nfl/500-dark/scoreboard/ne.png&w=64&h=64"));
  CHECK_EQ(s.possession, 0);
}

static GameSnapshot live(int us, int them, GameState st = GameState::IN) {
  GameSnapshot s;
  s.valid = true;
  s.state = st;
  s.event_id = "1";
  s.team_abbr = "DAL";
  s.opp_abbr = "PHI";
  s.team_score = us;
  s.opp_score = them;
  s.team_color = "002a5c";
  s.opp_color = "004c54";
  return s;
}

static void test_splash() {
  CHECK_EQ(decide_splash(live(0, 0), live(6, 0), true).text, std::string("TOUCHDOWN!"));
  CHECK_EQ(decide_splash(live(0, 0), live(7, 0), true).text, std::string("TOUCHDOWN!"));
  CHECK_EQ(decide_splash(live(0, 0), live(8, 0), true).text, std::string("TOUCHDOWN!"));
  CHECK_EQ(decide_splash(live(7, 0), live(10, 0), true).text, std::string("FIELD GOAL!"));
  CHECK_EQ(decide_splash(live(6, 0), live(7, 0), true).text, std::string("EXTRA POINT!"));
  CHECK_EQ(decide_splash(live(6, 0), live(8, 0), true).text, std::string("2-POINT!"));
  CHECK_EQ(decide_splash(live(0, 0), live(5, 0), true).text, std::string("SCORE!"));
  CHECK_EQ(decide_splash(live(0, 0), live(6, 0), true).color, (uint32_t) 0x002a5c);
  // Opponent
  CHECK_EQ(decide_splash(live(0, 0), live(0, 6), true).text, std::string("PHI TOUCHDOWN"));
  CHECK_EQ(decide_splash(live(0, 0), live(0, 3), true).text, std::string("PHI FIELD GOAL"));
  CHECK_EQ(decide_splash(live(0, 6), live(0, 7), true).text, std::string("PHI EXTRA POINT"));
  CHECK_EQ(decide_splash(live(0, 6), live(0, 8), true).text, std::string("PHI 2-POINT"));
  CHECK_EQ(decide_splash(live(0, 0), live(0, 6), true).color, (uint32_t) 0x004c54);
  CHECK_EQ(decide_splash(live(0, 0), live(0, 6), false).text, std::string(""));
  // Both scored between polls: ours wins
  CHECK_EQ(decide_splash(live(0, 0), live(7, 3), true).text, std::string("TOUCHDOWN!"));
  // No splash cases
  CHECK_EQ(decide_splash(GameSnapshot{}, live(7, 0), true).text, std::string(""));  // boot mid-game
  CHECK_EQ(decide_splash(live(0, 0, GameState::PRE), live(7, 0), true).text, std::string(""));
  CHECK_EQ(decide_splash(live(7, 0), live(7, 0), true).text, std::string(""));
  CHECK_EQ(decide_splash(live(7, 0), live(0, 0), true).text, std::string(""));  // correction
  GameSnapshot other = live(0, 0);
  other.event_id = "2";
  CHECK_EQ(decide_splash(other, live(7, 0), true).text, std::string(""));
  // Victory
  CHECK_EQ(decide_splash(live(21, 20), live(21, 20, GameState::POST), true).text, std::string("DAL WINS!"));
  CHECK_EQ(decide_splash(live(20, 21), live(20, 21, GameState::POST), true).text, std::string(""));
  CHECK_EQ(decide_splash(live(21, 20, GameState::POST), live(21, 20, GameState::POST), true).text, std::string(""));
}

static void test_status_text() {
  TickerOptions all;
  GameSnapshot s = live(24, 24);
  s.down_distance = "3rd & 10 at BOIS 25";
  s.short_detail = "15:00 - 4th";
  s.last_play = "End of 3rd quarter.";
  CHECK_EQ(status_text(s, all, ""), std::string("3rd & 10 at BOIS 25 | 15:00 - 4th | End of 3rd quarter."));
  TickerOptions no_clock = all;
  no_clock.clock = false;
  CHECK_EQ(status_text(s, no_clock, ""), std::string("3rd & 10 at BOIS 25 | End of 3rd quarter."));
  TickerOptions none;
  none.clock = none.down_distance = none.last_play = false;
  CHECK_EQ(status_text(s, none, ""), std::string("15:00 - 4th"));
  // Long play is truncated
  s.last_play = std::string(200, 'x');
  TickerOptions only_play;
  only_play.clock = only_play.down_distance = false;
  std::string t = status_text(s, only_play, "");
  CHECK_EQ(t.size(), (size_t) 160);
  CHECK(t.substr(157) == "...");
  // Halftime
  GameSnapshot h = live(14, 7);
  h.short_detail = "Halftime";
  h.team_record = "2-0";
  h.opp_record = "1-1";
  CHECK_EQ(status_text(h, all, ""), std::string("Halftime | DAL 2-0 | PHI 1-1"));
  // Pre
  GameSnapshot p = live(0, 0, GameState::PRE);
  p.odds = "SEA -3.5";
  p.over_under = "44.5";
  p.tv = "NBC";
  p.venue = "Lumen Field";
  CHECK_EQ(status_text(p, all, "Wed 7:20 PM"), std::string("Wed 7:20 PM | SEA -3.5 | O/U 44.5 | NBC | Lumen Field"));
  TickerOptions no_odds = all;
  no_odds.odds = false;
  CHECK_EQ(status_text(p, no_odds, "Wed 7:20 PM"), std::string("Wed 7:20 PM | Lumen Field"));
  // Post
  GameSnapshot f = live(21, 20, GameState::POST);
  f.team_record = "3-0";
  f.opp_record = "2-1";
  CHECK_EQ(status_text(f, all, ""), std::string("Final | DAL 3-0 | PHI 2-1"));
  GameSnapshot nf;
  CHECK_EQ(status_text(nf, all, ""), std::string("No upcoming game"));
}

static struct tm mk(int year, int yday, int wday, int hour, int min, int mon = 8, int mday = 5) {
  struct tm t{};
  t.tm_year = year - 1900;
  t.tm_yday = yday;
  t.tm_wday = wday;
  t.tm_hour = hour;
  t.tm_min = min;
  t.tm_mon = mon;
  t.tm_mday = mday;
  return t;
}

static void test_kickoff_label() {
  struct tm now = mk(2026, 247, 6, 10, 0);  // Sat Sep 5 10:00
  CHECK_EQ(kickoff_label(mk(2026, 247, 6, 14, 30), now), std::string("Today 2:30 PM"));
  CHECK_EQ(kickoff_label(mk(2026, 248, 0, 12, 0), now), std::string("Tomorrow 12:00 PM"));
  CHECK_EQ(kickoff_label(mk(2026, 251, 3, 19, 20), now), std::string("Wed 7:20 PM"));
  CHECK_EQ(kickoff_label(mk(2026, 256, 1, 0, 5, 8, 14), now), std::string("Sep 14 12:05 AM"));
}

static void test_upcoming() {
  std::vector<Upcoming> up;
  CHECK(parse_upcoming_str(slurp("fixtures/schedule_nfl_dal.json"), 6, 3, up));
  CHECK_EQ(up.size(), (size_t) 3);
  if (up.size() == 3) {
    CHECK_EQ(up[0].event_id, std::string("401872930"));
    CHECK_EQ(up[0].opp_abbr, std::string("NYG"));
    CHECK_EQ(up[0].opp_id, (uint32_t) 19);
    CHECK(!up[0].home);
    CHECK_EQ(up[0].tv, std::string("NBC"));
    CHECK_EQ(up[0].kickoff_epoch, parse_iso8601_z("2026-09-14T00:20Z"));
    CHECK_EQ(up[1].opp_abbr, std::string("WSH"));
    CHECK(up[1].home);
    CHECK_EQ(up[2].opp_abbr, std::string("BAL"));
    CHECK(up[2].neutral);
  }
  // College: the first event is already final and must be skipped
  up.clear();
  CHECK(parse_upcoming_str(slurp("fixtures/schedule_ncaa_bc.json"), 103, 4, up));
  CHECK_EQ(up.size(), (size_t) 4);
  if (up.size() >= 3) {
    CHECK_EQ(up[0].event_id, std::string("401858214"));
    CHECK_EQ(up[0].opp_abbr, std::string("RUTG"));
    CHECK(up[0].home);
    CHECK_EQ(up[0].tv, std::string("ESPN2"));
    CHECK_EQ(up[2].tv, std::string(""));  // no broadcast listed yet
  }
  CHECK_EQ(schedule_url(League::NFL, 6), std::string("https://site.api.espn.com/apis/site/v2/sports/football/nfl/teams/6/schedule"));
}

static void test_live_games() {
  std::string json = slurp("fixtures/scoreboard_ncaa_live.json");
  StringReader r(json);
  ScanResult scan;
  CHECK(parse_scan(r, scan));
  CHECK_EQ(scan.events, (size_t) 99);
  CHECK_EQ(scan.live.size(), (size_t) 18);
  CHECK_EQ(scan.finals.size(), (size_t) 45);
  CHECK_EQ(scan.week, 1);
  bool found = false;
  for (const auto &g : scan.live) {
    if (g.event_id == kLiveEvent) {
      found = true;
      CHECK_EQ(g.away_abbr, std::string("BOIS"));
      CHECK_EQ(g.home_abbr, std::string("ORE"));
      CHECK_EQ(g.away_id, kBoise);
      CHECK_EQ(g.home_id, kOregon);
      CHECK(g.group != 0);
      CHECK(g.state == GameState::IN);
      CHECK_EQ(g.kickoff_epoch, parse_iso8601_z("2026-09-05T19:30Z"));
    }
  }
  CHECK(found);
  found = false;
  for (const auto &g : scan.finals)
    found = found || (g.event_id == kPostEvent && g.away_id == kBallState && g.home_id == kOhioState);
  CHECK(found);
  // The earliest game that has not started: later today, kickoff and all.
  CHECK(scan.later.state == GameState::PRE);
  CHECK_EQ(scan.later.event_id, std::string("401856668"));
  CHECK_EQ(scan.later.kickoff_epoch, parse_iso8601_z("2026-09-05T23:00Z"));

  // Finals until midnight local: the fixture's week view also carries last
  // week's and Thursday's games, which must not rotate on Saturday.
  int64_t now = parse_iso8601_z("2026-09-05T22:00Z");
  CHECK_EQ(finals_today(scan.finals, now, -5 * 3600).size(), (size_t) 18);
  // 11:30 PM Central is still Saturday; 12:30 AM is Sunday and they are gone.
  CHECK_EQ(finals_today(scan.finals, parse_iso8601_z("2026-09-06T04:30Z"), -5 * 3600).size(), (size_t) 18);
  CHECK_EQ(finals_today(scan.finals, parse_iso8601_z("2026-09-06T05:30Z"), -5 * 3600).size(), (size_t) 0);
  CHECK_EQ(local_day(-1, 0), (int64_t) -1);

  // All-pre week: nothing live or final, the first game is next, week noted.
  std::string nfl = slurp("fixtures/scoreboard_nfl.json");
  StringReader rn(nfl);
  ScanResult none;
  CHECK(parse_scan(rn, none));
  CHECK_EQ(none.live.size(), (size_t) 0);
  CHECK_EQ(none.finals.size(), (size_t) 0);
  CHECK_EQ(none.week, 1);
  CHECK(none.later.state == GameState::PRE);
  CHECK_EQ(none.later.event_id, std::string(kNflEvent));
  CHECK_EQ(none.later.away_abbr, std::string("NE"));
  CHECK_EQ(none.later.home_abbr, std::string("SEA"));

  std::string junk = "{\"events\":[{\"id\":";
  StringReader rj(junk);
  ScanResult bad;
  CHECK(!parse_scan(rj, bad));
  std::string noevents = "{\"leagues\":[]}";
  StringReader rne(noevents);
  CHECK(!parse_scan(rne, bad));
  // Whitespace around the key, as NWS pretty-prints its JSON.
  std::string spaced = "{\"week\": {\"number\": 3}, \"events\" :\n [ ]}";
  StringReader rs(spaced);
  CHECK(parse_scan(rs, bad));
  CHECK_EQ(bad.events, (size_t) 0);

  CHECK_EQ(week_url(League::NFL, 0), std::string("https://site.api.espn.com/apis/site/v2/sports/football/nfl/scoreboard"));
  CHECK_EQ(week_url(League::NFL, 4), std::string("https://site.api.espn.com/apis/site/v2/sports/football/nfl/scoreboard?week=4"));
  CHECK_EQ(week_url(League::NCAA, 0), std::string("https://site.api.espn.com/apis/site/v2/sports/football/college-football/scoreboard?groups=80"));
  CHECK_EQ(week_url(League::NCAA, 5), std::string("https://site.api.espn.com/apis/site/v2/sports/football/college-football/scoreboard?groups=80&week=5"));
  CHECK(scan_url(League::NFL, parse_iso8601_z("2026-09-14T01:00Z")).find("dates=20260913") != std::string::npos);
  CHECK(scan_url(League::NCAA, 0).find("groups=80") != std::string::npos);
  // neutral splashes name both sides
  CHECK_EQ(decide_splash(live(0, 0), live(6, 0), true, true).text, std::string("DAL TOUCHDOWN"));
  CHECK_EQ(decide_splash(live(0, 0), live(0, 3), true, true).text, std::string("PHI FIELD GOAL"));
  CHECK_EQ(decide_splash(live(20, 21), live(20, 21, GameState::POST), true, true).text, std::string("PHI WINS!"));
}

static void test_live_precedence() {
  LiveInputs in;
  CHECK(live_precedence(in) == LiveShow::FETCH_NEXT);  // nothing known yet: ask for the week
  in.next = NextState::NONE;
  CHECK(live_precedence(in) == LiveShow::IDLE);
  in.next = NextState::FOUND;
  CHECK(live_precedence(in) == LiveShow::NEXT_GAME);
  in.finals_today = 3;
  CHECK(live_precedence(in) == LiveShow::FINAL_TODAY);
  in.later_today = true;
  CHECK(live_precedence(in) == LiveShow::LATER_TODAY);  // a kickoff to come beats a final
  in.any_live = true;
  CHECK(live_precedence(in) == LiveShow::LIVE);
  // The v1.4.0 behavior, kept as a setting: live first, else the saved team.
  LiveInputs mine;
  mine.my_team_fallback = true;
  mine.later_today = true;
  mine.finals_today = 2;
  CHECK(live_precedence(mine) == LiveShow::MY_TEAM);
  mine.any_live = true;
  CHECK(live_precedence(mine) == LiveShow::LIVE);
  // Finals need no next-game lookup.
  LiveInputs fin;
  fin.finals_today = 1;
  CHECK(live_precedence(fin) == LiveShow::FINAL_TODAY);

  TickerOptions o;
  GameSnapshot pre = live(0, 0, GameState::PRE);
  pre.team_abbr = "UGA";
  pre.opp_abbr = "BAMA";
  pre.odds = "BAMA -3";
  pre.tv = "CBS";
  CHECK_EQ(live_ticker(LiveShow::LATER_TODAY, "college ", pre, o, "Today 2:30 PM", "x"),
           std::string("Next college game: UGA at BAMA, 2:30 PM | BAMA -3 | CBS"));
  CHECK_EQ(live_ticker(LiveShow::NEXT_GAME, "", pre, o, "Thu 6:30 PM", "x"),
           std::string("Next game: UGA at BAMA, Thu 6:30 PM | BAMA -3 | CBS"));
  TickerOptions no_odds;
  no_odds.odds = false;
  CHECK_EQ(live_ticker(LiveShow::NEXT_GAME, "NFL ", pre, no_odds, "Thu 6:30 PM", "x"),
           std::string("Next NFL game: UGA at BAMA, Thu 6:30 PM"));
  GameSnapshot fin_g = live(24, 10, GameState::POST);
  CHECK_EQ(live_ticker(LiveShow::FINAL_TODAY, "college ", fin_g, o, "", "Final | DAL 2-0"),
           std::string("Today: Final | DAL 2-0"));
  CHECK_EQ(live_ticker(LiveShow::IDLE, "college ", GameSnapshot{}, o, "", "No upcoming game"),
           std::string("No college games scheduled"));
  CHECK_EQ(live_ticker(LiveShow::NONE, "NFL ", GameSnapshot{}, o, "", "No upcoming game"),
           std::string("Looking for a live NFL game"));
  CHECK_EQ(live_ticker(LiveShow::LIVE, "", live(7, 0), o, "", "base"), std::string("base"));
  // Never an apology, never the saved team's name.
  CHECK(live_ticker(LiveShow::MY_TEAM, "college ", pre, o, "", "Sat 2:30 PM").find("showing") == std::string::npos);
}

static void test_clock_text() {
  GameSnapshot s = live(0, 0);
  s.display_clock = "0:42";
  s.period = 2;
  s.short_detail = "1:10 - 2nd";  // stale formatted text
  CHECK_EQ(clock_text(s), std::string("0:42 2nd"));
  s.period = 5;
  CHECK_EQ(clock_text(s), std::string("0:42 OT"));
  s.period = 6;
  CHECK_EQ(clock_text(s), std::string("0:42 2OT"));
  s.period = 2;
  s.display_clock = "0:00";
  s.short_detail = "Halftime";
  CHECK_EQ(clock_text(s), std::string("Halftime"));
  s.short_detail = "End of 3rd";
  CHECK_EQ(clock_text(s), std::string("End of 3rd"));
  s.display_clock = "";
  s.short_detail = "12:34 - 4th";
  CHECK_EQ(clock_text(s), std::string("12:34 4th"));
}

static void test_color() {
  CHECK_EQ(parse_color("002a5c"), (uint32_t) 0x002a5c);
  CHECK_EQ(parse_color(""), (uint32_t) 0xFFFFFF);
  CHECK_EQ(parse_color("zzzzzz"), (uint32_t) 0xFFFFFF);
}

// ---- favorites mode: lock-on, release, collisions, playlist -------------------
static FavGame fav(GameState st, int64_t kick, int64_t final_at = 0) {
  FavGame g;
  g.valid = true;
  g.state = st;
  g.kickoff_epoch = kick;
  g.final_epoch = final_at;
  return g;
}

static void test_favorites() {
  FavRules r;
  r.lockon_s = 15 * 60;
  r.release_s = 30;
  r.alternate = false;
  r.rotate_s = 5 * 60;
  r.dwell_s = 10;
  const int64_t T = 1000000;

  // lock window: only within lockon_s before kickoff, while IN, or release_s after the final
  CHECK(!fav_locked(fav(GameState::PRE, T + 16 * 60), T, r));
  CHECK(fav_locked(fav(GameState::PRE, T + 15 * 60), T, r));
  CHECK(fav_locked(fav(GameState::PRE, T - 60), T, r));  // ESPN lag: still PRE past kickoff
  CHECK(fav_locked(fav(GameState::IN, T - 3600), T, r));
  CHECK(fav_locked(fav(GameState::POST, T - 3 * 3600, T - 29), T, r));
  CHECK(!fav_locked(fav(GameState::POST, T - 3 * 3600, T - 30), T, r));
  CHECK(!fav_locked(fav(GameState::POST, T - 3 * 3600, 0), T, r));  // already final when fetched
  CHECK(!fav_locked(FavGame{}, T, r));

  // playlist: nothing locked, cycle valid entries in slot order every dwell_s
  std::vector<FavGame> idle = {fav(GameState::PRE, T + 86400), FavGame{}, fav(GameState::PRE, T + 2 * 86400),
                               fav(GameState::PRE, T + 3 * 86400)};
  FavChoice c = pick_favorite(idle, T, r, -1, 0, -1);
  CHECK_EQ(c.index, 0);
  CHECK(!c.locked);
  c = pick_favorite(idle, T + 9, r, 0, T, -1);
  CHECK_EQ(c.index, 0);  // dwell not over
  c = pick_favorite(idle, T + 10, r, 0, T, -1);
  CHECK_EQ(c.index, 2);  // skips the empty slot
  c = pick_favorite(idle, T + 30, r, 3, T + 20, -1);
  CHECK_EQ(c.index, 0);  // wraps
  // a current entry that became invalid moves on at once
  c = pick_favorite(idle, T + 1, r, 1, T, -1);
  CHECK_EQ(c.index, 2);
  // nothing valid at all
  std::vector<FavGame> none = {FavGame{}, FavGame{}, FavGame{}, FavGame{}};
  c = pick_favorite(none, T, r, -1, 0, -1);
  CHECK_EQ(c.index, -1);

  // one locked: hold it regardless of dwell
  std::vector<FavGame> one = {fav(GameState::PRE, T + 86400), fav(GameState::IN, T - 600), fav(GameState::PRE, T + 86400),
                              FavGame{}};
  c = pick_favorite(one, T + 100, r, 0, T, -1);
  CHECK_EQ(c.index, 1);
  CHECK(c.locked);
  c = pick_favorite(one, T + 5000, r, 1, T + 100, -1);
  CHECK_EQ(c.index, 1);

  // collision, stick: the higher slot wins even when the lower one is shown
  std::vector<FavGame> two = {fav(GameState::IN, T - 600), fav(GameState::IN, T - 600), fav(GameState::PRE, T + 86400),
                              FavGame{}};
  c = pick_favorite(two, T + 5000, r, 1, T, -1);
  CHECK_EQ(c.index, 0);
  CHECK(c.locked);
  // collision, alternate: hold for rotate_s, then the next locked one, wrapping
  r.alternate = true;
  c = pick_favorite(two, T + 299, r, 0, T, -1);
  CHECK_EQ(c.index, 0);
  c = pick_favorite(two, T + 300, r, 0, T, -1);
  CHECK_EQ(c.index, 1);
  c = pick_favorite(two, T + 600, r, 1, T + 300, -1);
  CHECK_EQ(c.index, 0);
  // alternate with the shown entry not locked jumps to a locked one at once
  c = pick_favorite(two, T + 1, r, 2, T, -1);
  CHECK_EQ(c.index, 0);
  r.alternate = false;

  // pin: a pinned locked entry beats priority; a pinned idle entry only seeds the playlist
  c = pick_favorite(two, T + 5000, r, 0, T, 1);
  CHECK_EQ(c.index, 1);
  CHECK(c.locked);
  c = pick_favorite(idle, T + 5, r, 2, T, 2);
  CHECK_EQ(c.index, 2);
  CHECK(!c.locked);
  c = pick_favorite(idle, T + 10, r, 2, T, 2);
  CHECK_EQ(c.index, 3);  // dwell over, playlist continues past the pin
  // a pin on an empty slot is ignored
  c = pick_favorite(two, T + 5000, r, 0, T, 3);
  CHECK_EQ(c.index, 0);
}

// A schedule parsed from the team endpoint does not know its own league, so
// the caller has to stamp it. Without the stamp a college favorite polls the
// NFL scoreboard and never finds its game.
static void test_schedule_league() {
  std::string json = slurp("fixtures/team_ncaa_bc.json");
  Schedule s;
  CHECK(parse_team_str(json, s));
  CHECK(s.valid);
  CHECK_EQ((int) s.league, 0);  // documents the gap: 0 is League::NFL

  adopt_league(s, League::NCAA);
  CHECK_EQ((int) s.league, (int) League::NCAA);

  std::string url = scoreboard_url((League) s.league, s.group, s.kickoff_epoch);
  CHECK(url.find("/college-football/") != std::string::npos);
  CHECK(url.find("/nfl/") == std::string::npos);
}

// Refresh Now, and the six hour schedule cycle. SCHEDULE_INTERVAL is 6 hours;
// the helper takes it as an argument so the test does not depend on the device
// constant.
static const uint32_t kSix = 6u * 60u * 60u * 1000u;

static void test_schedule_due() {
  // Nothing fetched yet: the panel has to read the team endpoint.
  CHECK(schedule_due(false, false, 1000, 0, kSix));

  // Fresh schedule, nothing asked for: poll the game we already know about.
  CHECK(!schedule_due(true, false, kSix, kSix - 1000, kSix));

  // Older than the cycle: read it again.
  CHECK(schedule_due(true, false, 2 * kSix, kSix - 1000, kSix));
  CHECK(schedule_due(true, false, kSix, 0, kSix));  // exactly due

  // Refresh Now forces the re-read however fresh the schedule is, and whatever
  // the uptime. The second case is the v1.3.x bug: zeroing the timestamp on a
  // still valid schedule left the test as millis() >= 6 h, so Refresh Now did
  // nothing for the first six hours after a restart.
  CHECK(schedule_due(true, true, 1000, 500, kSix));
  CHECK(schedule_due(true, true, 60u * 1000u, 0, kSix));

  // Without the force flag that same state is not due: this is what made the
  // old refresh_now() a no-op, and it stays true so the force flag is the only
  // thing carrying Refresh Now.
  CHECK(!schedule_due(true, false, 60u * 1000u, 0, kSix));

  // millis() wraps every 49 days. Unsigned subtraction still gives the real
  // age, so a wrap must not force a spurious re-read.
  uint32_t before_wrap = 0xFFFFFFFFu - 1000u;
  CHECK(!schedule_due(true, false, 2000, before_wrap, kSix));  // ~3 s old
  CHECK(schedule_due(true, false, before_wrap + 1000u + kSix, before_wrap, kSix));
}

static void test_team_schedule() {
  // BC's first game is already final: a 34-15 loss at Cincinnati.
  std::string json = slurp("fixtures/schedule_ncaa_bc.json");
  TeamSchedule bc;
  CHECK(parse_team_schedule(json, 103, 4, bc));
  CHECK(bc.valid);
  CHECK(bc.last.valid);
  CHECK_EQ(bc.last.opp_abbr, std::string("CIN"));
  CHECK_EQ(bc.last.us, 15);
  CHECK_EQ(bc.last.them, 34);
  CHECK(!bc.last.home);
  CHECK_EQ(result_line(bc.last), std::string("L 15-34 at CIN"));
  CHECK_EQ(bc.group, (uint32_t) 1);
  CHECK_EQ(bc.upcoming.size(), (size_t) 4);
  CHECK_EQ(bc.upcoming[0].opp_abbr, std::string("RUTG"));

  // DAL: nothing played yet, record and standing from the team block.
  std::string dal_json = slurp("fixtures/schedule_nfl_dal.json");
  TeamSchedule dal;
  CHECK(parse_team_schedule(dal_json, 6, 2, dal));
  CHECK(!dal.last.valid);
  CHECK_EQ(result_line(dal.last), std::string(""));
  CHECK_EQ(dal.record, std::string("0-0"));
  CHECK_EQ(dal.color, std::string("002a5c"));
  CHECK_EQ(dal.standing, std::string("1st in NFC East"));
  CHECK_EQ(dal.group, (uint32_t) 1);
  CHECK_EQ(dal.upcoming.size(), (size_t) 2);
  CHECK_EQ(dal.upcoming[0].opp_abbr, std::string("NYG"));

  GameResult r;
  r.valid = true;
  r.opp_abbr = "WSH";
  r.us = 37;
  r.them = 20;
  r.home = true;
  CHECK_EQ(result_line(r), std::string("W 37-20 vs WSH"));
  r.home = false;
  r.neutral = true;
  r.us = 20;
  CHECK_EQ(result_line(r), std::string("T 20-20 vs WSH"));
}

static void test_standings() {
  std::string nfl = slurp("fixtures/standings_nfl_nfc_east.json");
  StringReader r(nfl);
  Standings st;
  CHECK(parse_standings(r, 20, st));
  CHECK_EQ(st.title, std::string("NFC East"));
  CHECK_EQ(st.rows.size(), (size_t) 4);
  CHECK_EQ(st.rows[0].abbr, std::string("PHI"));
  CHECK_EQ(st.rows[0].record, std::string("2-0"));
  CHECK_EQ(st.rows[2].abbr, std::string("DAL"));
  CHECK_EQ(st.rows[2].team_id, (uint32_t) 6);
  CHECK_EQ(st.rows[3].record, std::string("0-2"));

  // A conference: the short name, overall record out of ~100 stats per team.
  std::string acc = slurp("fixtures/standings_ncaa_acc.json");
  StringReader ra(acc);
  Standings conf;
  CHECK(parse_standings(ra, 20, conf));
  CHECK_EQ(conf.title, std::string("ACC"));
  CHECK_EQ(conf.rows.size(), (size_t) 6);
  CHECK_EQ(conf.rows[0].abbr, std::string("MIA"));
  CHECK_EQ(conf.rows[0].record, std::string("3-0"));
  CHECK_EQ(conf.rows[5].abbr, std::string("CLEM"));
  CHECK_EQ(conf.rows[5].record, std::string("2-1"));
  // The cap stops the download early.
  StringReader rc(acc);
  Standings capped;
  CHECK(parse_standings(rc, 2, capped));
  CHECK_EQ(capped.rows.size(), (size_t) 2);
  CHECK(rc.total() < acc.size() / 2);

  std::string stub = "{\"fullViewLink\":{\"text\":\"Full Standings\"}}";
  StringReader rs(stub);
  Standings none;
  CHECK(!parse_standings(rs, 20, none));
  CHECK_EQ(standings_url(League::NFL, 1), std::string("https://site.api.espn.com/apis/v2/sports/football/nfl/standings?group=1"));
  CHECK_EQ(standings_url(League::NCAA, 8), std::string("https://site.api.espn.com/apis/v2/sports/football/college-football/standings?group=8"));
}

static void test_weather() {
  std::string pts = slurp("fixtures/nws_points.json");
  std::string grid;
  CHECK(nws::parse_points(pts, grid));
  CHECK_EQ(grid, std::string("FWD/86,112"));
  CHECK_EQ(nws::hourly_url(grid), std::string("https://api.weather.gov/gridpoints/FWD/86,112/forecast/hourly"));
  CHECK_EQ(nws::points_url(32.95371f, -96.89032f), std::string("https://api.weather.gov/points/32.9537,-96.8903"));
  std::string bad_grid;
  std::string not_found = "{\"status\":404}";
  CHECK(!nws::parse_points(not_found, bad_grid));

  // Pretty-printed like the service sends it; 30 hours in the fixture.
  std::string hourly = slurp("fixtures/nws_hourly.json");
  StringReader r(hourly);
  nws::Weather w;
  CHECK(nws::parse_hourly(r, w));
  CHECK(w.valid);
  CHECK_EQ(w.temp, 99);
  CHECK_EQ(w.condition, std::string("Mostly Sunny"));
  CHECK_EQ(w.high, 100);
  CHECK_EQ(w.low, 76);
  CHECK_EQ(w.unit, std::string("F"));
  CHECK(r.total() < hourly.size());  // the rest of the week is never read
  StringReader r2(hourly);
  nws::Weather hour;
  CHECK(nws::parse_hourly(r2, hour, 1));
  CHECK_EQ(hour.high, 99);
  CHECK_EQ(hour.low, 99);
  std::string empty = "{\"properties\": {\"periods\": []}}";
  StringReader re(empty);
  nws::Weather none;
  CHECK(!nws::parse_hourly(re, none));
}

static void test_idle() {
  using namespace idle;
  // Only the enabled screens that have something to show, in order, wrapping.
  uint8_t on = bit(CLOCK) | bit(COUNTDOWN) | bit(WEATHER);
  uint8_t have = bit(CLOCK) | bit(COUNTDOWN) | bit(STANDINGS);
  CHECK_EQ(next_screen(on, have, -1), (int) CLOCK);
  CHECK_EQ(next_screen(on, have, CLOCK), (int) COUNTDOWN);
  CHECK_EQ(next_screen(on, have, COUNTDOWN), (int) CLOCK);  // weather has no data, standings is off
  CHECK_EQ(next_screen(on, have | bit(WEATHER), COUNTDOWN), (int) WEATHER);
  // Clock turned off and countdown has nothing: still the clock, never dark.
  CHECK_EQ(next_screen(bit(COUNTDOWN), bit(CLOCK), CLOCK), (int) CLOCK);
  CHECK_EQ(next_screen(bit(RECORD), ALL_SCREENS, RECORD), (int) RECORD);
  CHECK_EQ(DEFAULT_SCREENS, (uint8_t) 3);
  CHECK_EQ(std::string(screen_name(STANDINGS)), std::string("standings"));
  CHECK_EQ(std::string(screen_name(9)), std::string(""));

  CHECK_EQ(countdown_text(3 * 86400 + 14 * 3600 + 22 * 60 + 5, true), std::string("3d 14h 22m"));
  CHECK_EQ(countdown_text(3 * 86400 + 14 * 3600 + 22 * 60 + 5, false), std::string("3d 14h"));
  CHECK_EQ(countdown_text(14 * 3600 + 2 * 60, false), std::string("14h 02m"));
  CHECK_EQ(countdown_text(22 * 60 + 15, true), std::string("22:15"));
  CHECK_EQ(countdown_text(59, false), std::string("0:59"));
  CHECK_EQ(countdown_text(0, true), std::string("Kickoff"));

  struct tm t = {};
  t.tm_hour = 19;
  t.tm_min = 42;
  t.tm_wday = 2;
  t.tm_mon = 8;
  t.tm_mday = 23;
  CHECK_EQ(clock_text(t), std::string("7:42"));
  t.tm_hour = 0;
  t.tm_min = 5;
  CHECK_EQ(clock_text(t), std::string("12:05"));
  CHECK_EQ(date_text(t), std::string("Tue Sep 23"));

  CHECK_EQ(standings_page(4, 4, 100, 20), (size_t) 0);
  CHECK_EQ(standings_page(17, 4, 0, 20), (size_t) 0);
  CHECK_EQ(standings_page(17, 4, 20, 20), (size_t) 1);
  CHECK_EQ(standings_page(17, 4, 99, 20), (size_t) 4);
  CHECK_EQ(standings_page(17, 4, 100, 20), (size_t) 0);  // five pages, wraps
}

int main() {
  test_urls();
  test_iso();
  test_team_parse();
  test_scoreboard_live();
  test_scoreboard_post();
  test_scoreboard_pre_nfl();
  test_splash();
  test_status_text();
  test_kickoff_label();
  test_live_games();
  test_live_precedence();
  test_upcoming();
  test_clock_text();
  test_color();
  test_favorites();
  test_schedule_league();
  test_schedule_due();
  test_team_schedule();
  test_standings();
  test_weather();
  test_idle();
  printf("%d checks, %d failures\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
