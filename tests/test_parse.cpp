// Host tests for the ESPN parser and game logic. Build with `make` in tests/.
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

#include "espn_parse.h"

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
  CHECK_EQ(dark_logo("https://a.espncdn.com/i/teamlogos/ncaa/500/68.png"), std::string("https://a.espncdn.com/i/teamlogos/ncaa/500-dark/68.png"));
  CHECK_EQ(dark_logo("https://a.espncdn.com/i/teamlogos/nfl/500/scoreboard/sea.png"), std::string("https://a.espncdn.com/i/teamlogos/nfl/500-dark/scoreboard/sea.png"));
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
  CHECK_EQ(s.team_logo, std::string("https://a.espncdn.com/i/teamlogos/ncaa/500-dark/68.png"));
  CHECK_EQ(s.opp_logo, std::string("https://a.espncdn.com/i/teamlogos/ncaa/500-dark/2483.png"));
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
  CHECK_EQ(s.team_logo, std::string("https://a.espncdn.com/i/teamlogos/nfl/500-dark/scoreboard/ne.png"));
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

static void test_color() {
  CHECK_EQ(parse_color("002a5c"), (uint32_t) 0x002a5c);
  CHECK_EQ(parse_color(""), (uint32_t) 0xFFFFFF);
  CHECK_EQ(parse_color("zzzzzz"), (uint32_t) 0xFFFFFF);
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
  test_color();
  printf("%d checks, %d failures\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
