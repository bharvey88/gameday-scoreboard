// Host tests for the ESPN parser and game logic. Build with `make` in tests/.
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "espn_parse.h"
#include "favorites.h"
#include "logo_lru.h"
#include "startup.h"
#include "timezones.h"

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
  std::vector<LiveGame> games;
  CHECK(parse_live_games(json, games));
  CHECK(games.size() >= 1);
  bool found = false;
  for (const auto &g : games) {
    if (g.event_id == kLiveEvent) {
      found = true;
      CHECK_EQ(g.away_abbr, std::string("BOIS"));
      CHECK_EQ(g.home_abbr, std::string("ORE"));
      CHECK_EQ(g.away_id, kBoise);
      CHECK(g.group != 0);
    }
  }
  CHECK(found);
  std::vector<LiveGame> none;
  std::string nfl = slurp("fixtures/scoreboard_nfl.json");
  CHECK(parse_live_games(nfl, none));
  CHECK_EQ(none.size(), (size_t) 0);
  CHECK(scan_url(League::NFL, parse_iso8601_z("2026-09-14T01:00Z")).find("dates=20260913") != std::string::npos);
  CHECK(scan_url(League::NCAA, 0).find("groups=80") != std::string::npos);
  // neutral splashes name both sides
  CHECK_EQ(decide_splash(live(0, 0), live(6, 0), true, true).text, std::string("DAL TOUCHDOWN"));
  CHECK_EQ(decide_splash(live(0, 0), live(0, 3), true, true).text, std::string("PHI FIELD GOAL"));
  CHECK_EQ(decide_splash(live(20, 21), live(20, 21, GameState::POST), true, true).text, std::string("PHI WINS!"));
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

static void test_team_cache() {
  Schedule d;
  CHECK(parse_team_str(slurp("fixtures/team_nfl_dal.json"), d));
  // Saved the evening before the game.
  int64_t saved = d.kickoff_epoch - 20 * 3600;
  TeamCache c;
  CHECK(team_cache_save(d, League::NFL, 6, saved, c));

  // Next morning, same team: straight to the scoreboard.
  Schedule s;
  CHECK(team_cache_load(c, League::NFL, 6, saved + 12 * 3600, s));
  CHECK(s.valid);
  CHECK_EQ(s.event_id, std::string("401872930"));
  CHECK_EQ(s.kickoff_epoch, d.kickoff_epoch);
  CHECK_EQ(s.group, d.group);
  CHECK_EQ(s.league, (uint8_t) League::NFL);

  // Another team, or the same id in the other league: read the team endpoint.
  CHECK(!team_cache_load(c, League::NFL, 21, saved + 60, s));
  CHECK(!team_cache_load(c, League::NCAA, 6, saved + 60, s));

  // More than a day old, or saved "in the future" (a clock that jumped back).
  CHECK(team_cache_load(c, League::NFL, 6, saved + TEAM_CACHE_MAX_AGE, s));
  CHECK(!team_cache_load(c, League::NFL, 6, saved + TEAM_CACHE_MAX_AGE + 1, s));
  CHECK(!team_cache_load(c, League::NFL, 6, saved - 1, s));

  // Kicked off more than four hours ago: that game is over.
  TeamCache late;
  CHECK(team_cache_save(d, League::NFL, 6, d.kickoff_epoch + 3600, late));
  CHECK(team_cache_load(late, League::NFL, 6, d.kickoff_epoch + TEAM_CACHE_GAME_OVER, s));
  CHECK(!team_cache_load(late, League::NFL, 6, d.kickoff_epoch + TEAM_CACHE_GAME_OVER + 1, s));

  // College keeps its conference group for the scoreboard request.
  Schedule bc;
  CHECK(parse_team_str(slurp("fixtures/team_ncaa_bc.json"), bc));
  TeamCache cc;
  CHECK(team_cache_save(bc, League::NCAA, 103, bc.kickoff_epoch - 3600, cc));
  Schedule sc;
  CHECK(team_cache_load(cc, League::NCAA, 103, bc.kickoff_epoch, sc));
  CHECK_EQ(sc.event_id, std::string("401856777"));
  CHECK_EQ(sc.group, (uint32_t) 1);
  CHECK_EQ(sc.league, (uint8_t) League::NCAA);

  // Nothing to keep: no next game, an id too long for the record, no clock.
  Schedule none;
  none.valid = true;
  TeamCache n;
  CHECK(!team_cache_save(none, League::NFL, 6, saved, n));
  Schedule longid = d;
  longid.event_id = "1234567890123456";  // 16 characters, no room for the end
  CHECK(!team_cache_save(longid, League::NFL, 6, saved, n));
  CHECK(!team_cache_save(d, League::NFL, 6, 0, n));

  // A blank record (nothing saved yet reads back as zeros) is never used.
  TeamCache blank{};
  CHECK(!team_cache_load(blank, League::NFL, 0, 0, s));
}

static void test_boot_hold() {
  // Holding: no board yet, inside the cap.
  CHECK(boot_hold(false, 1000, 1000));
  CHECK(boot_hold(false, 1000, 1000 + BOOT_HOLD_CAP_MS - 1));
  // The cap ends it whether or not a board came.
  CHECK(!boot_hold(false, 1000, 1000 + BOOT_HOLD_CAP_MS));
  // The first scoreboard ends it at once.
  CHECK(!boot_hold(true, 1000, 1500));
  // No hold running.
  CHECK(!boot_hold(false, 0, 1500));
  // millis() wraps every 49 days: the age is still right.
  uint32_t before_wrap = 0xFFFFFFFFu - 2000u;
  CHECK(boot_hold(false, before_wrap, 3000));
  CHECK(!boot_hold(false, before_wrap, before_wrap + BOOT_HOLD_CAP_MS));
}

static void test_rotate_minutes() {
  // 1 minute is a real choice on the device page and in the app. Treating it
  // as corrupt at boot reset the mode to My team.
  CHECK(espn::rotate_minutes_valid(1));
  CHECK(espn::rotate_minutes_valid(30));
  CHECK(!espn::rotate_minutes_valid(0));
  CHECK(!espn::rotate_minutes_valid(31));
  CHECK(espn::rotate_minutes_valid(espn::kRotateDefaultMinutes));
  CHECK(espn::kRotateDefaultMinutes == 1);
  CHECK(espn::clamp_rotate_minutes(0) == 1);
  CHECK(espn::clamp_rotate_minutes(-5) == 1);
  CHECK(espn::clamp_rotate_minutes(1) == 1);
  CHECK(espn::clamp_rotate_minutes(45) == 30);
}

// MLB single-event documents recorded 2026-10-07 (see fixtures/README.md).
static const uint32_t kBraves = 15, kDodgers = 19, kWhiteSox = 4, kBrewers = 8;

static GameSnapshot mlb_event(const char *file, uint32_t team) {
  GameSnapshot s;
  CHECK(parse_event_str(slurp(file), Sport::BASEBALL, team, s));
  return s;
}

static void test_mlb_urls() {
  CHECK_EQ(team_url(League::MLB, 15), std::string("https://site.api.espn.com/apis/site/v2/sports/baseball/mlb/teams/15"));
  CHECK_EQ(event_url(League::MLB, "401908016"),
           std::string("https://site.api.espn.com/apis/site/v2/sports/baseball/mlb/scoreboard/401908016"));
  CHECK_EQ(team_logo_url(League::MLB, 15, "ATL"),
           std::string("https://a.espncdn.com/combiner/i?img=/i/teamlogos/mlb/500-dark/atl.png&w=64&h=64"));
  CHECK_EQ(std::string(league_key(League::MLB)), std::string("mlb"));
  CHECK(league_by_key("mlb") != nullptr && league_by_key("mlb")->per_event);
  CHECK(league_by_key("cfl") == nullptr);
  // A West Coast game still on at 2:30 AM Eastern is listed under the day it
  // started; the next morning's scan moves on to the new day.
  CHECK(scan_url(League::MLB, parse_iso8601_z("2026-10-08T06:30Z")).find("dates=20261007") != std::string::npos);
  CHECK(scan_url(League::MLB, parse_iso8601_z("2026-10-08T13:00Z")).find("dates=20261008") != std::string::npos);
  CHECK_EQ(scan_url(League::MLB, parse_iso8601_z("2026-10-08T13:00Z")),
           std::string("https://site.api.espn.com/apis/site/v2/sports/baseball/mlb/scoreboard?dates=20261008"));
  CHECK(league_sport(League::MLB) == Sport::BASEBALL);
  CHECK(league_sport(League::NCAA) == Sport::FOOTBALL);
  bool found = false;
  for (size_t i = 0; i < kTeamCount; i++) {
    if (kTeams[i].league == League::MLB && kTeams[i].espn_id == kBraves) {
      CHECK_EQ(team_option(kTeams[i]), std::string("MLB: Atlanta Braves"));
      found = true;
    }
  }
  CHECK(found);
}

static void test_mlb_team() {
  Schedule s;
  CHECK(parse_team_str(slurp("fixtures/team_mlb_atl.json"), s));
  CHECK_EQ(s.event_id, std::string("401908016"));
  CHECK_EQ(s.kickoff_epoch, parse_iso8601_z("2026-10-07T22:00Z"));
  CHECK_EQ(s.team_color, std::string("0c2340"));
  CHECK_EQ(s.team_record, std::string("94-68"));
}

static void test_mlb_live() {
  // Top 7th, LAD batting, runner on second, 2-1 count, two out. ATL is home.
  GameSnapshot t = mlb_event("fixtures/event_mlb_top.json", kBraves);
  CHECK(t.valid);
  CHECK(t.sport == Sport::BASEBALL);
  CHECK(t.state == GameState::IN);
  CHECK(t.team_home);
  CHECK_EQ(t.team_abbr, std::string("ATL"));
  CHECK_EQ(t.opp_abbr, std::string("LAD"));
  CHECK_EQ(t.team_score, 1);
  CHECK_EQ(t.opp_score, 1);
  CHECK(t.mlb.half == Half::TOP);
  CHECK_EQ((int) t.mlb.balls, 2);
  CHECK_EQ((int) t.mlb.strikes, 1);
  CHECK_EQ((int) t.mlb.outs, 2);
  CHECK_EQ((int) t.mlb.bases, (int) Baseball::SECOND);
  CHECK_EQ(t.possession, 2);  // the visitors bat in the top half
  CHECK_EQ(t.mlb.batter, std::string("M. Muncy"));
  CHECK_EQ(t.mlb.pitcher, std::string("D. Fuentes"));
  CHECK_EQ(t.mlb.team_hits, 2);
  CHECK_EQ(t.mlb.opp_hits, 3);
  CHECK_EQ(t.mlb.team_errors, 2);
  CHECK_EQ(t.mlb.opp_errors, 0);
  CHECK_EQ(t.mlb.series, std::string("LAD lead series 2-1"));
  CHECK_EQ(t.mlb.play_type, std::string("Ball"));
  CHECK_EQ(t.last_play, std::string("Pitch 3 : Ball 2"));
  CHECK_EQ(t.team_logo, std::string("https://a.espncdn.com/combiner/i?img=/i/teamlogos/mlb/500-dark/scoreboard/atl.png&w=64&h=64"));
  CHECK_EQ(clock_text(t), std::string("Top 7th"));
  CHECK_EQ(baseball_count(t), std::string("2-1"));
  TickerOptions o;
  o.clock = false;
  CHECK_EQ(status_text(t, o, ""), std::string("2-1, 2 outs | P D. Fuentes, AB M. Muncy | Pitch 3 : Ball 2"));
  TickerOptions quiet = o;
  quiet.down_distance = quiet.last_play = false;
  CHECK_EQ(status_text(t, quiet, ""), std::string("Top 7th"));

  // The same game from the Dodgers' side: they bat.
  GameSnapshot d = mlb_event("fixtures/event_mlb_top.json", kDodgers);
  CHECK(!d.team_home);
  CHECK_EQ(d.possession, 1);
  CHECK_EQ(d.mlb.team_hits, 3);

  // Bottom 6th: the Braves bat, one out.
  GameSnapshot b = mlb_event("fixtures/event_mlb_bottom.json", kBraves);
  CHECK(b.mlb.half == Half::BOTTOM);
  CHECK_EQ(b.possession, 1);
  CHECK_EQ((int) b.mlb.outs, 1);
  CHECK_EQ(status_text(b, o, ""), std::string("1-1, 1 out | P E. Henriquez, AB L. Thomas | Pitch 2 : Ball 1"));

  // End of an inning: ESPN keeps a stale count, the panel must not show it.
  GameSnapshot e = mlb_event("fixtures/event_mlb_end.json", kBraves);
  CHECK(e.mlb.half == Half::END);
  CHECK_EQ((int) e.mlb.balls, -1);
  CHECK_EQ((int) e.mlb.outs, -1);
  CHECK_EQ((int) e.mlb.bases, 0);
  CHECK_EQ(e.possession, 0);
  CHECK_EQ(baseball_count(e), std::string(""));
  CHECK_EQ(clock_text(e), std::string("End 5th"));
  CHECK_EQ(status_text(e, o, ""), std::string("ATL 1 H 2 E, LAD 2 H 0 E | LAD lead series 2-1"));
}

static void test_mlb_pre_post() {
  GameSnapshot p = mlb_event("fixtures/event_mlb_pre.json", kBrewers);
  CHECK(p.state == GameState::PRE);
  CHECK_EQ(p.mlb.team_probable, std::string("R. Gasser"));
  CHECK_EQ(p.mlb.opp_probable, std::string("W. Buehler"));
  CHECK_EQ((int) p.mlb.balls, -1);
  CHECK_EQ(p.possession, 0);
  CHECK_EQ(p.odds, std::string("SD -112"));
  CHECK_EQ(p.tv, std::string("FS1"));
  TickerOptions o;
  std::string pre = status_text(p, o, "Today 10:00 PM");
  CHECK(pre.rfind("Today 10:00 PM | MIL leads series 2-1 | MIL R. Gasser vs SD W. Buehler | SD -112 | ", 0) == 0);
  CHECK(pre.find(" | FS1 | Petco Park") != std::string::npos);

  GameSnapshot f = mlb_event("fixtures/event_mlb_final.json", kWhiteSox);
  CHECK(f.state == GameState::POST);
  CHECK(!f.team_winner);
  CHECK_EQ(f.team_score, 3);
  CHECK_EQ(f.opp_score, 9);
  CHECK_EQ(status_text(f, o, ""), std::string("Final | CHW 6 H 0 E, CLE 11 H 0 E | CHW lead series 2-1"));
  f.short_detail = "Final/10";
  CHECK(status_text(f, o, "").rfind("Final/10 | ", 0) == 0);

  GameSnapshot x;
  CHECK(!parse_event_str(slurp("fixtures/event_mlb_top.json"), Sport::BASEBALL, kWhiteSox, x));  // not in it
  CHECK(!parse_event_str("{nope", Sport::BASEBALL, kBraves, x));
}

static void test_mlb_splash() {
  GameSnapshot prev = mlb_event("fixtures/event_mlb_top.json", kBraves);
  auto after = [&](int us, int them, const char *play) {
    GameSnapshot c = prev;
    c.team_score += us;
    c.opp_score += them;
    c.mlb.play_type = play;
    return c;
  };
  CHECK_EQ(decide_splash(prev, after(1, 0, "Play Result"), true).text, std::string("RUN!"));
  CHECK_EQ(decide_splash(prev, after(3, 0, "Play Result"), true).text, std::string("3 RUNS!"));
  CHECK_EQ(decide_splash(prev, after(4, 0, "Home Run"), true).text, std::string("HOME RUN!"));
  CHECK_EQ(decide_splash(prev, after(1, 0, "Play Result"), true).color, (uint32_t) 0x0c2340);
  CHECK_EQ(decide_splash(prev, after(0, 1, "Play Result"), true).text, std::string("LAD SCORES"));
  CHECK_EQ(decide_splash(prev, after(0, 2, "Play Result"), true).text, std::string("LAD 2 RUNS"));
  CHECK_EQ(decide_splash(prev, after(0, 1, "Home Run"), true).text, std::string("LAD HOME RUN"));
  CHECK_EQ(decide_splash(prev, after(0, 1, "Home Run"), false).text, std::string(""));
  GameSnapshot hr = after(0, 2, "Play Result");
  hr.last_play = "Muncy homered to right (402 feet), Freeman scored.";
  CHECK_EQ(decide_splash(prev, hr, true).text, std::string("LAD HOME RUN"));
  // Live modes: neither side is ours.
  CHECK_EQ(decide_splash(prev, after(1, 0, "Play Result"), true, true).text, std::string("ATL SCORES"));
  CHECK_EQ(decide_splash(prev, after(0, 0, "Ball"), true).text, std::string(""));
  GameSnapshot fin = after(2, 0, "Play Result");
  fin.state = GameState::POST;
  GameSnapshot was = fin;
  was.state = GameState::IN;
  CHECK_EQ(decide_splash(was, fin, true).text, std::string("ATL WINS!"));
}

// Favorites list: the today-only filter and the season horizon.
static void test_favorites_filters() {
  const int64_t day = 86400;
  const int64_t now = 1791000000;  // a Wednesday afternoon, say
  FavRules r;
  r.day_start = now - 15 * 3600;
  r.day_end = r.day_start + day;
  auto card = [](GameState st, int64_t kick, int64_t horizon) {
    FavGame g;
    g.valid = true;
    g.state = st;
    g.kickoff_epoch = kick;
    g.horizon_s = horizon;
    return g;
  };
  const int64_t h = 10 * day;
  FavGame tonight = card(GameState::PRE, now + 4 * 3600, h);
  FavGame saturday = card(GameState::PRE, now + 3 * day, h);
  FavGame spring = card(GameState::PRE, now + 150 * day, h);       // MLB in December
  FavGame bye_week = card(GameState::PRE, now + 13 * day, 0);      // football: no horizon
  FavGame old_final = card(GameState::POST, now - 40 * day, h);    // last season's final
  FavGame live = card(GameState::IN, now - 3600, h);
  FavGame final_today = card(GameState::POST, r.day_start + 3600, h);

  CHECK(fav_idle_ok(tonight, now, r, false));
  CHECK(fav_idle_ok(saturday, now, r, false));
  CHECK(!fav_idle_ok(spring, now, r, false));
  CHECK(fav_idle_ok(bye_week, now, r, false));
  CHECK(!fav_idle_ok(old_final, now, r, false));
  CHECK(fav_idle_ok(tonight, now, r, true));
  CHECK(!fav_idle_ok(saturday, now, r, true));
  CHECK(fav_idle_ok(live, now, r, true));
  CHECK(fav_idle_ok(final_today, now, r, true));
  CHECK(!fav_idle_ok(FavGame{}, now, r, false));

  // The playlist skips the out-of-season card.
  std::vector<FavGame> games = {saturday, spring, tonight};
  FavChoice c = pick_favorite(games, now, r, 0, now - 20, -1);
  CHECK_EQ(c.index, 2);
  c = pick_favorite(games, now, r, 2, now - 20, -1);
  CHECK_EQ(c.index, 0);
  // Today only: just tonight's game, and it stays up past the dwell.
  r.today_only = true;
  c = pick_favorite(games, now, r, 2, now - 20, -1);
  CHECK_EQ(c.index, 2);
  CHECK(!c.locked);
  // Nothing today: the filter relaxes to the season horizon...
  std::vector<FavGame> quiet = {saturday, spring};
  c = pick_favorite(quiet, now, r, -1, 0, -1);
  CHECK_EQ(c.index, 0);
  c = pick_favorite(quiet, now, r, 0, now - 20, -1);
  CHECK_EQ(c.index, 0);
  // ...and with every team out of season, any card beats a blank panel.
  std::vector<FavGame> winter = {spring, old_final};
  c = pick_favorite(winter, now, r, -1, 0, -1);
  CHECK_EQ(c.index, 0);
  c = pick_favorite(winter, now, r, 0, now - 20, -1);
  CHECK_EQ(c.index, 1);
  // A remote press shows its card for the dwell even when today-only would
  // skip it, then the playlist moves on.
  c = pick_favorite(games, now, r, 0, now, 0);
  CHECK_EQ(c.index, 0);
  c = pick_favorite(games, now, r, 0, now - 20, 0);
  CHECK_EQ(c.index, 2);
  // A locked game still wins over everything.
  r.today_only = false;
  std::vector<FavGame> mixed = {saturday, live};
  c = pick_favorite(mixed, now, r, 0, now, -1);
  CHECK_EQ(c.index, 1);
  CHECK(c.locked);
}

// Soccer single-event documents (see fixtures/README.md): Brazilian Serie A
// recorded live 2026-10-08, MLS and Premier League pre-game and finals, a
// Champions League final on penalties and a World Cup game after extra time.
static const uint32_t kArsenal = 359, kLeeds = 357, kSunderland = 366, kMiami = 20232, kDcUnited = 193,
                      kInter = 1936, kCorinthians = 874, kVasco = 3454, kMirassol = 9169, kBragantino = 6079,
                      kPsg = 160, kArgentina = 202, kCruzeiro = 2022;

static GameSnapshot soccer_event(const char *file, uint32_t team) {
  GameSnapshot s;
  CHECK(parse_event_str(slurp(file), Sport::SOCCER, team, s));
  return s;
}

static void test_soccer_urls() {
  CHECK_EQ(team_url(League::EPL, kArsenal), std::string("https://site.api.espn.com/apis/site/v2/sports/soccer/eng.1/teams/359"));
  CHECK_EQ(team_url(League::MLS, kMiami), std::string("https://site.api.espn.com/apis/site/v2/sports/soccer/usa.1/teams/20232"));
  CHECK_EQ(event_url(League::EPL, "401879268"),
           std::string("https://site.api.espn.com/apis/site/v2/sports/soccer/eng.1/scoreboard/401879268"));
  // A soccer schedule lists past results unless asked for fixtures.
  CHECK_EQ(schedule_url(League::EPL, kArsenal),
           std::string("https://site.api.espn.com/apis/site/v2/sports/soccer/eng.1/teams/359/schedule?fixture=true"));
  CHECK_EQ(schedule_url(League::MLB, 15), std::string("https://site.api.espn.com/apis/site/v2/sports/baseball/mlb/teams/15/schedule"));
  CHECK_EQ(team_logo_url(League::EPL, kArsenal, "ARS"),
           std::string("https://a.espncdn.com/combiner/i?img=/i/teamlogos/soccer/500-dark/359.png&w=64&h=64"));
  CHECK_EQ(team_logo_url(League::MLS, kMiami, "MIA"),
           std::string("https://a.espncdn.com/combiner/i?img=/i/teamlogos/soccer/500-dark/20232.png&w=64&h=64"));
  CHECK_EQ(std::string(league_key(League::EPL)), std::string("epl"));
  CHECK_EQ(std::string(league_key(League::MLS)), std::string("mls"));
  CHECK(league_by_key("epl") != nullptr && league_by_key("epl")->per_event && league_by_key("epl")->season_list);
  CHECK(league_by_key("mls") != nullptr && league_by_key("mls")->logo_by_id);
  CHECK(league_sport(League::EPL) == Sport::SOCCER);
  CHECK(league_sport(League::MLS) == Sport::SOCCER);
  int epl = 0, mls = 0;
  for (size_t i = 0; i < kTeamCount; i++) {
    const Team &t = kTeams[i];
    epl += t.league == League::EPL;
    mls += t.league == League::MLS;
    if (t.league == League::EPL && t.espn_id == kArsenal)
      CHECK_EQ(team_option(t), std::string("EPL: Arsenal"));
    if (t.league == League::MLS && t.espn_id == kMiami)
      CHECK_EQ(team_option(t), std::string("MLS: Inter Miami CF"));
  }
  CHECK_EQ(epl, 20);
  CHECK_EQ(mls, 30);
}

static void test_soccer_team() {
  Schedule s;
  CHECK(parse_team_str(slurp("fixtures/team_epl_ars.json"), s));
  CHECK_EQ(s.event_id, std::string("401879268"));
  CHECK_EQ(s.kickoff_epoch, parse_iso8601_z("2026-10-10T11:30Z"));
  CHECK_EQ(s.team_color, std::string("e20520"));
  CHECK_EQ(s.team_record, std::string("4-0-1"));
  CHECK(parse_team_str(slurp("fixtures/team_mls_mia.json"), s));
  CHECK_EQ(s.event_id, std::string("761847"));
  CHECK_EQ(s.team_record, std::string("12-10-5"));

  // The fixture list feeds Up next the same way football's schedule does.
  std::vector<Upcoming> up;
  CHECK(parse_upcoming_str(slurp("fixtures/schedule_mls_mia.json"), kMiami, 4, up));
  CHECK_EQ(up.size(), (size_t) 2);
  CHECK_EQ(up[0].event_id, std::string("761847"));
  CHECK_EQ(up[0].opp_id, kDcUnited);
  CHECK_EQ(up[0].opp_abbr, std::string("DC"));
  CHECK(up[0].home);
  CHECK_EQ(up[0].tv, std::string("Apple TV"));
  CHECK_EQ(up[1].opp_abbr, std::string("NYC"));
  CHECK_EQ(up[1].kickoff_epoch, parse_iso8601_z("2026-10-14T23:30Z"));
}

static void test_soccer_pre() {
  GameSnapshot a = soccer_event("fixtures/event_epl_pre.json", kArsenal);
  CHECK(a.valid);
  CHECK(a.sport == Sport::SOCCER);
  CHECK(a.state == GameState::PRE);
  CHECK(a.team_home);
  CHECK_EQ(a.opp_abbr, std::string("LEE"));
  CHECK_EQ(a.team_record, std::string("4-0-1"));  // W-D-L, the order of ESPN's standings
  // ESPN sends the newest result first ("LWWWW": Arsenal lost their last
  // game); the ticker prints it oldest first like a league table.
  CHECK_EQ(a.soc.team_form, std::string("WWWWL"));
  CHECK_EQ(a.soc.opp_form, std::string("DDLWD"));
  CHECK_EQ(a.soc.team_line, std::string("-275"));
  CHECK_EQ(a.soc.draw_line, std::string("+425"));
  CHECK_EQ(a.soc.opp_line, std::string("+750"));
  CHECK(a.soc.goals.empty());
  CHECK_EQ(a.team_logo, std::string("https://a.espncdn.com/combiner/i?img=/i/teamlogos/soccer/500-dark/359.png&w=64&h=64"));
  TickerOptions o;
  CHECK_EQ(status_text(a, o, "Sat 7:30 AM"),
           std::string("Sat 7:30 AM | Form ARS WWWWL, LEE DDLWD | ARS -275, draw +425, LEE +750 | O/U 2.5 | USA Net | "
                       "Emirates Stadium"));
  // From the visitors' side the line turns around.
  GameSnapshot l = soccer_event("fixtures/event_epl_pre.json", kLeeds);
  CHECK(!l.team_home);
  CHECK_EQ(l.soc.team_line, std::string("+750"));
  CHECK_EQ(l.soc.opp_line, std::string("-275"));

  GameSnapshot m = soccer_event("fixtures/event_mls_pre.json", kMiami);
  CHECK_EQ(m.team_record, std::string("12-10-5"));
  CHECK_EQ(status_text(m, o, "Sat 7:30 PM"),
           std::string("Sat 7:30 PM | Form MIA DDWDL, DC DWDLD | MIA -270, draw +425, DC +500 | O/U 3.5 | Apple TV | "
                       "Nu Stadium"));
  TickerOptions no_odds = o;
  no_odds.odds = false;
  CHECK_EQ(status_text(m, no_odds, ""),
           std::string("Scheduled | Form MIA DDWDL, DC DWDLD | Nu Stadium"));
  // Only the favorite's line without the three-way one.
  GameSnapshot partial = m;
  partial.soc.opp_line.clear();
  CHECK_EQ(status_text(partial, o, "Sat 7:30 PM").find("MIA -270, draw +425 | O/U 3.5") != std::string::npos, true);

  GameSnapshot none;
  none.sport = Sport::SOCCER;
  CHECK_EQ(status_text(none, o, ""), std::string("No upcoming game"));
}

static void test_soccer_live() {
  TickerOptions o;
  o.clock = false;  // as the component asks: the clock has its own row

  // First half, no goals yet: an empty situation row.
  GameSnapshot fh = soccer_event("fixtures/event_soccer_first_half.json", kCruzeiro);
  CHECK(fh.state == GameState::IN);
  CHECK_EQ(fh.soc.status, std::string("STATUS_FIRST_HALF"));
  CHECK_EQ(clock_text(fh), std::string("25'"));
  CHECK_EQ(soccer_situation(fh), std::string(""));
  CHECK_EQ(status_text(fh, o, ""),
           std::string("Cards CRU 1Y, SAO 1Y | Possession CRU 64.1%, SAO 35.9% | Shots (on target) CRU 4 (0), SAO 4 (1) | "
                       "24' Yellow Card: G. Rojas (CRU)"));
  TickerOptions with_clock;
  with_clock.down_distance = with_clock.last_play = false;
  CHECK_EQ(status_text(fh, with_clock, ""), std::string("25'"));

  // Half time, Vasco (away) lead 1-0.
  GameSnapshot h = soccer_event("fixtures/event_soccer_halftime.json", kVasco);
  CHECK(h.state == GameState::IN);
  CHECK_EQ(h.soc.status, std::string("STATUS_HALFTIME"));
  CHECK_EQ(h.display_clock, std::string("45'+5'"));  // the clock stops where the half ended
  CHECK_EQ(clock_text(h), std::string("HT"));
  CHECK_EQ(soccer_situation(h), std::string("28' Lescano"));
  CHECK_EQ(status_text(h, o, ""),
           std::string("Half time | Goals VAS 28' Lescano | Possession VAS 54.8%, BOT 45.2% | "
                       "Shots (on target) VAS 8 (4), BOT 6 (1) | W-D-L VAS 8-7-12, BOT 9-8-11"));

  // Stoppage time, Internacional (home) 2-1 up. ESPN's status name flips
  // between STATUS_SECOND_HALF and STATUS_IN_PROGRESS from poll to poll.
  GameSnapshot s = soccer_event("fixtures/event_soccer_stoppage.json", kInter);
  CHECK_EQ(s.soc.status, std::string("STATUS_IN_PROGRESS"));
  CHECK_EQ(clock_text(s), std::string("90'+2'"));
  CHECK_EQ(s.team_score, 2);
  CHECK_EQ(s.opp_score, 1);
  CHECK_EQ(s.soc.goals.size(), (size_t) 3);
  CHECK(s.soc.goals[0].ours);
  CHECK(!s.soc.goals[2].ours);
  CHECK_EQ(s.soc.goals[2].name, std::string("Andre"));  // "André", folded for the panel font
  CHECK_EQ(s.soc.team_yellows, 3);
  CHECK_EQ(s.soc.opp_yellows, 4);
  CHECK_EQ(s.soc.team_reds + s.soc.opp_reds, 0);
  CHECK_EQ(soccer_situation(s), std::string("80' Andre"));
  CHECK_EQ(s.soc.last_event, std::string("85' Yellow Card: R. Garro (COR)"));
  CHECK_EQ(status_text(s, o, ""),
           std::string("Goals INT 1' Vitinho, 77' Alan Patrick; COR 80' Andre | Cards INT 3Y, COR 4Y | "
                       "Possession INT 28%, COR 72% | Shots (on target) INT 17 (7), COR 21 (6) | "
                       "85' Yellow Card: R. Garro (COR)"));
  TickerOptions quiet = o;
  quiet.down_distance = quiet.last_play = false;
  CHECK_EQ(status_text(s, quiet, ""), std::string("90'+2'"));

  // Second half with a red card: Mirassol (away) down to ten.
  GameSnapshot r = soccer_event("fixtures/event_soccer_second_half.json", kMirassol);
  CHECK_EQ(clock_text(r), std::string("90'"));
  CHECK_EQ(r.soc.team_reds, 1);
  CHECK_EQ(r.soc.opp_reds, 0);
  CHECK_EQ(r.soc.last_event, std::string("70' Goal: Andre Luis (MIR)"));
  // 14 characters is too wide for the row: the last word of the name stays.
  CHECK_EQ(soccer_situation(r), std::string("70' Luis"));
  CHECK(status_text(r, o, "").rfind("Goals MIR 70' Andre Luis; BRA 51' Eduardo Sasha | Cards MIR 1R | ", 0) == 0);
  GameSnapshot b = soccer_event("fixtures/event_soccer_second_half.json", kBragantino);
  CHECK_EQ(b.soc.team_reds, 0);
  CHECK_EQ(b.soc.opp_reds, 1);

  // Full time in the same game as the stoppage-time poll.
  GameSnapshot f = soccer_event("fixtures/event_soccer_full_time.json", kInter);
  CHECK(f.state == GameState::POST);
  CHECK(f.team_winner);
  CHECK_EQ(clock_text(f), std::string("FT"));
  CHECK_EQ(status_text(f, o, ""),
           std::string("Full time | Goals INT 1' Vitinho, 77' Alan Patrick; COR 80' Andre | W-D-L INT 6-10-12, COR 8-8-12"));
  CHECK_EQ(decide_splash(s, f, true).text, std::string("INT WINS!"));
  CHECK_EQ(decide_splash(s, f, true).color, (uint32_t) 0xC60000);
  GameSnapshot sc = soccer_event("fixtures/event_soccer_stoppage.json", kCorinthians);
  GameSnapshot fc = soccer_event("fixtures/event_soccer_full_time.json", kCorinthians);
  CHECK_EQ(decide_splash(sc, fc, true).text, std::string(""));
  CHECK_EQ(decide_splash(sc, fc, true, true).text, std::string("INT WINS!"));  // live modes: the winner, either side
}

static void test_soccer_finals() {
  TickerOptions o;
  // Premier League: Arsenal win 2-0 away with a stoppage-time penalty; Sunderland had a man sent off.
  GameSnapshot a = soccer_event("fixtures/event_epl_final.json", kArsenal);
  CHECK(a.state == GameState::POST);
  CHECK(a.team_winner);
  CHECK_EQ(a.soc.opp_reds, 1);
  CHECK(a.soc.goals.back().penalty);
  CHECK_EQ(status_text(a, o, ""),
           std::string("Full time | Goals ARS 58' Guimaraes, 90'+7' Saka (P) | Red cards SUN 1 | W-D-L ARS 4-0-0, SUN 1-1-2"));
  // "90'+7' Saka (P)" is 15 characters: the penalty mark gives way.
  CHECK_EQ(soccer_situation(a), std::string("90'+7' Saka"));
  CHECK_EQ(soccer_situation(a, 16), std::string("90'+7' Saka (P)"));
  GameSnapshot sun = soccer_event("fixtures/event_epl_final.json", kSunderland);
  CHECK_EQ(sun.soc.team_reds, 1);

  // MLS: a 2-2 draw. Both sides have winner false and nobody gets a splash.
  GameSnapshot d = soccer_event("fixtures/event_mls_final.json", kMiami);
  CHECK(!d.team_winner);
  CHECK_EQ(d.team_score, 2);
  CHECK_EQ(d.opp_score, 2);
  CHECK_EQ(clock_text(d), std::string("FT"));
  CHECK_EQ(status_text(d, o, ""),
           std::string("Full time | Goals MIA 24' Messi, 74' Suarez; SD 12' Dreyer, 82' Dreyer | W-D-L MIA 12-10-4, SD 8-7-11"));
  GameSnapshot live = d;
  live.state = GameState::IN;
  CHECK_EQ(decide_splash(live, d, true).text, std::string(""));
  CHECK_EQ(decide_splash(live, d, true, true).text, std::string(""));

  // Champions League final on penalties: 1-1, PSG win the shootout 4-3.
  GameSnapshot p = soccer_event("fixtures/event_soccer_pens.json", kArsenal);
  CHECK_EQ(p.soc.status, std::string("STATUS_FINAL_PEN"));
  CHECK_EQ(p.soc.team_shootout, 3);
  CHECK_EQ(p.soc.opp_shootout, 4);
  CHECK_EQ(p.soc.goals.size(), (size_t) 2);  // shootout kicks are not goals
  CHECK_EQ(clock_text(p), std::string("PENS"));
  CHECK_EQ(p.soc.last_event, std::string("Shootout Penalty - Scored: L. Beraldo (PSG)"));
  CHECK_EQ(status_text(p, o, ""),
           std::string("PSG win 4-3 on penalties | UEFA Champions League, Final | Goals ARS 6' Havertz; PSG 65' Dembele (P)"));
  GameSnapshot pl = p;
  pl.state = GameState::IN;
  pl.soc.status = "STATUS_SHOOTOUT";
  CHECK_EQ(clock_text(pl), std::string("PENS 3-4"));
  CHECK_EQ(decide_splash(pl, p, true).text, std::string(""));
  CHECK_EQ(decide_splash(pl, p, true, true).text, std::string("PSG WINS!"));
  GameSnapshot psg = soccer_event("fixtures/event_soccer_pens.json", kPsg);
  GameSnapshot psg_live = psg;
  psg_live.state = GameState::IN;
  CHECK_EQ(decide_splash(psg_live, psg, true).text, std::string("PSG WINS!"));

  // World Cup round of 32 after extra time, with an own goal at 111'.
  GameSnapshot e = soccer_event("fixtures/event_soccer_aet.json", kArgentina);
  CHECK_EQ(clock_text(e), std::string("AET"));
  CHECK(e.soc.goals.back().own_goal);
  CHECK(e.soc.goals.back().ours);  // an own goal counts for the side it helped
  CHECK_EQ(e.soc.goals.back().name, std::string("Borges"));
  CHECK_EQ(status_text(e, o, ""),
           std::string("After extra time | FIFA World Cup, Round of 32 | Goals ARG 29' Messi, 92' Martinez, "
                       "111' Borges (OG); CPV 59' Duarte, 103' Lopes Cabral"));
  CHECK_EQ(soccer_situation(e), std::string("111' Borges"));
  GameSnapshot og = e;
  og.soc.goals.back().clock = "55'";
  CHECK_EQ(soccer_situation(og), std::string("55' Borges"));  // "55' Borges OG" is 13
  CHECK_EQ(soccer_situation(og, 13), std::string("55' Borges OG"));
  og.soc.goals.back().name = "Hany";
  CHECK_EQ(soccer_situation(og), std::string("55' Hany OG"));

  // Extra time while it is played (no recording yet: built from the final).
  GameSnapshot et = e;
  et.state = GameState::IN;
  et.soc.status = "STATUS_SECOND_HALF_EXTRA_TIME";
  et.period = 3;
  et.display_clock = "105'+1'";
  CHECK_EQ(clock_text(et), std::string("ET 105'+1'"));
  et.period = 4;
  et.display_clock = "120'+12'";
  CHECK_EQ(clock_text(et), std::string("120'+12'"));
  et.period = 3;
  et.soc.status = "STATUS_HALFTIME_ET";
  CHECK_EQ(clock_text(et), std::string("ET HT"));
  GameSnapshot x;
  CHECK(!parse_event_str(slurp("fixtures/event_soccer_aet.json"), Sport::SOCCER, kArsenal, x));  // not in it
}

static void test_soccer_splash() {
  GameSnapshot prev = soccer_event("fixtures/event_soccer_stoppage.json", kInter);
  auto after = [&](int us, int them) {
    GameSnapshot c = prev;
    c.team_score += us;
    c.opp_score += them;
    return c;
  };
  CHECK_EQ(decide_splash(prev, after(1, 0), true).text, std::string("GOAL!"));
  CHECK_EQ(decide_splash(prev, after(1, 0), true).color, (uint32_t) 0xC60000);
  CHECK_EQ(decide_splash(prev, after(0, 1), true).text, std::string("COR GOAL"));
  CHECK_EQ(decide_splash(prev, after(0, 1), false).text, std::string(""));
  CHECK_EQ(decide_splash(prev, after(1, 0), true, true).text, std::string("INT GOAL"));
  CHECK_EQ(decide_splash(prev, after(0, 0), true).text, std::string(""));
  CHECK_EQ(decide_splash(prev, after(-1, 0), true).text, std::string(""));  // a goal taken back
  GameSnapshot pre = prev;
  pre.state = GameState::PRE;
  CHECK_EQ(decide_splash(pre, after(1, 0), true).text, std::string(""));
  for (const char *word : {"GOAL!", "REMO GOAL", "INT WINS!"})
    CHECK(strlen(word) <= 10);
}

static void test_soccer_text_helpers() {
  CHECK_EQ(ascii_fold("Guimar\xC3\xA3" "es"), std::string("Guimaraes"));
  CHECK_EQ(ascii_fold("Gy\xC3\xB6keres"), std::string("Gyokeres"));
  CHECK_EQ(ascii_fold("\xC3\x98" "degaard"), std::string("Odegaard"));
  CHECK_EQ(ascii_fold("Demb\xC3\xA9l\xC3\xA9"), std::string("Dembele"));
  CHECK_EQ(ascii_fold("\xC5\x81ukasz \xC5\x9E" "ahin"), std::string("Lukasz Sahin"));
  CHECK_EQ(ascii_fold("\xC8\x98tefan"), std::string("Stefan"));
  CHECK_EQ(ascii_fold("O\xE2\x80\x99Reilly"), std::string("O'Reilly"));
  CHECK_EQ(ascii_fold("Stra\xC3\x9F" "e \xC3\x86"), std::string("Strasse AE"));
  CHECK_EQ(ascii_fold("Plain ASCII 1-0"), std::string("Plain ASCII 1-0"));
  CHECK_EQ(ascii_fold("bad\xFF\xC3"), std::string("bad"));
  CHECK_EQ(ascii_fold("\xE6\x97\xA5\xE6\x9C\xAC"), std::string(""));
  CHECK_EQ(soccer_board_record("6-10-12"), std::string("6-10-12"));
  CHECK_EQ(soccer_board_record("12-10-12"), std::string("46 pts"));  // too wide under a logo
  CHECK_EQ(soccer_board_record(""), std::string(""));
}

// A club's next game across its league and cups (team endpoints recorded
// 2026-10-08: Arsenal's league game is on 10/10, its Champions League game
// on 10/13, and it is not in the Europa League).
static void test_soccer_cups() {
  CHECK_EQ(team_url(League::EPL, kArsenal, "uefa.champions"),
           std::string("https://site.api.espn.com/apis/site/v2/sports/soccer/uefa.champions/teams/359"));
  CHECK_EQ(event_url(League::EPL, "401915417", "uefa.champions"),
           std::string("https://site.api.espn.com/apis/site/v2/sports/soccer/uefa.champions/scoreboard/401915417"));
  CHECK_EQ(team_url(League::MLS, kMiami, "concacaf.champions"),
           std::string("https://site.api.espn.com/apis/site/v2/sports/soccer/concacaf.champions/teams/20232"));
  CHECK_EQ(event_url(League::EPL, "401879268", nullptr), event_url(League::EPL, "401879268"));
  CHECK_EQ(team_url(League::NFL, 6, nullptr), team_url(League::NFL, 6));
  CHECK_EQ(std::string(league_cup(League::EPL, 0)), std::string("uefa.champions"));
  CHECK_EQ(std::string(league_cup(League::EPL, 2)), std::string("uefa.europa.conf"));
  CHECK(league_cup(League::EPL, 3) == nullptr);
  CHECK_EQ(std::string(league_cup(League::MLS, 0)), std::string("concacaf.champions"));
  CHECK_EQ(league_cup_count(League::EPL), (size_t) 3);
  CHECK_EQ(league_cup_count(League::MLS), (size_t) 1);
  CHECK_EQ(league_cup_count(League::NFL), (size_t) 0);
  CHECK_EQ(league_cup_count(League::MLB), (size_t) 0);

  std::vector<Schedule> comps(4);
  CHECK(parse_team_str(slurp("fixtures/team_epl_ars.json"), comps[0]));
  CHECK(parse_team_str(slurp("fixtures/team_ucl_ars.json"), comps[1]));
  CHECK(parse_team_str(slurp("fixtures/team_uel_ars.json"), comps[2]));
  for (size_t i = 0; i < comps.size(); i++) {
    adopt_league(comps[i], League::EPL);
    adopt_comp(comps[i], i == 0 ? nullptr : league_cup(League::EPL, i - 1));
  }
  CHECK_EQ(comps[1].event_id, std::string("401915417"));
  CHECK_EQ(comps[1].kickoff_epoch, parse_iso8601_z("2026-10-13T19:00Z"));
  CHECK(comps[2].valid);  // not in the Europa League: a good read with no game
  CHECK(comps[2].event_id.empty());
  CHECK(!comps[3].valid);  // the Conference League not read yet

  int64_t league_ko = parse_iso8601_z("2026-10-10T11:30Z");
  // Before the league game: it is first.
  CHECK_EQ(pick_schedule(comps, parse_iso8601_z("2026-10-08T00:00Z"), ""), 0);
  // While it is on, and just after, ESPN still names it: it stays.
  CHECK_EQ(pick_schedule(comps, league_ko + 2 * 3600, ""), 0);
  // Seen to finish, or long over: the cup game is next.
  CHECK_EQ(pick_schedule(comps, league_ko + 2 * 3600, "401879268"), 1);
  CHECK_EQ(pick_schedule(comps, league_ko + 5 * 3600, ""), 1);
  // A cup game first in the week.
  std::vector<Schedule> early = comps;
  early[1].kickoff_epoch = league_ko - 3 * 86400;
  CHECK_EQ(pick_schedule(early, parse_iso8601_z("2026-10-06T00:00Z"), ""), 1);
  // Same kickoff: the league's read wins.
  early[1].kickoff_epoch = league_ko;
  CHECK_EQ(pick_schedule(early, 0, ""), 0);
  // Nothing else: a game that is over is still better than none.
  std::vector<Schedule> only_over = {comps[0], comps[2]};
  CHECK_EQ(pick_schedule(only_over, league_ko + 10 * 3600, ""), 0);
  // No read names a game (an off season): none.
  std::vector<Schedule> none = {comps[2], comps[3]};
  CHECK_EQ(pick_schedule(none, 0, ""), -1);
  CHECK_EQ(pick_schedule(std::vector<Schedule>{}, 0, ""), -1);
  // The league read had no game but a cup does (MLS's winter, the CONCACAF Champions Cup).
  std::vector<Schedule> cup_only = {comps[2], comps[1]};
  CHECK_EQ(pick_schedule(cup_only, 0, ""), 1);

  // The boot cache has no room for the cup: a cup game saves a blank record
  // that never loads, so an older league game cannot come back either.
  TeamCache c{};
  c.team_id = 99;
  CHECK(team_cache_save(comps[1], League::EPL, kArsenal, league_ko, c));
  CHECK_EQ(c.team_id, (uint32_t) 0);
  Schedule loaded;
  CHECK(!team_cache_load(c, League::EPL, kArsenal, league_ko, loaded));
  CHECK(team_cache_save(comps[0], League::EPL, kArsenal, league_ko - 86400, c));
  CHECK(team_cache_load(c, League::EPL, kArsenal, league_ko - 3600, loaded));
  CHECK_EQ(loaded.event_id, std::string("401879268"));
  CHECK(loaded.comp_slug == nullptr);
}

// NBA (preseason) and WNBA (semifinal) single-event documents recorded
// 2026-10-07 (see fixtures/README.md).
static const uint32_t kPacers = 11, kWolves = 16, kThunder = 25, kBlazers = 22, kDream = 20, kLiberty = 9;

static GameSnapshot hoops_event(const char *file, uint32_t team) {
  GameSnapshot s;
  CHECK(parse_event_str(slurp(file), Sport::BASKETBALL, team, s));
  return s;
}

static void test_nba_urls() {
  CHECK_EQ(team_url(League::NBA, 11), std::string("https://site.api.espn.com/apis/site/v2/sports/basketball/nba/teams/11"));
  CHECK_EQ(event_url(League::WNBA, "401918297"),
           std::string("https://site.api.espn.com/apis/site/v2/sports/basketball/wnba/scoreboard/401918297"));
  CHECK_EQ(team_logo_url(League::NBA, 11, "IND"),
           std::string("https://a.espncdn.com/combiner/i?img=/i/teamlogos/nba/500-dark/ind.png&w=64&h=64"));
  CHECK_EQ(team_logo_url(League::WNBA, 129689, "GS"),
           std::string("https://a.espncdn.com/combiner/i?img=/i/teamlogos/wnba/500-dark/gs.png&w=64&h=64"));
  CHECK_EQ(std::string(league_key(League::NBA)), std::string("nba"));
  CHECK_EQ(std::string(league_key(League::WNBA)), std::string("wnba"));
  for (const char *key : {"nba", "wnba"}) {
    const LeagueInfo *l = league_by_key(key);
    CHECK(l != nullptr && l->per_event && !l->season_list && l->linger_min == 30);
    CHECK(l != nullptr && l->sport == Sport::BASKETBALL);
  }
  int nba = 0, wnba = 0;
  for (size_t i = 0; i < kTeamCount; i++) {
    const Team &t = kTeams[i];
    nba += t.league == League::NBA;
    wnba += t.league == League::WNBA;
    if (t.league == League::NBA && t.espn_id == kPacers)
      CHECK_EQ(team_option(t), std::string("NBA: Indiana Pacers"));
    if (t.league == League::WNBA && t.espn_id == kLiberty)
      CHECK_EQ(team_option(t), std::string("WNBA: New York Liberty"));
  }
  CHECK_EQ(nba, 30);
  CHECK(wnba >= 13);
}

static void test_nba_team() {
  Schedule s;
  CHECK(parse_team_str(slurp("fixtures/team_nba_ind.json"), s));
  CHECK_EQ(s.event_id, std::string("401914123"));
  CHECK_EQ(s.kickoff_epoch, parse_iso8601_z("2026-10-07T23:00Z"));
  CHECK_EQ(s.team_color, std::string("0c2340"));
  CHECK_EQ(s.team_record, std::string("0-0"));
  Schedule w;
  CHECK(parse_team_str(slurp("fixtures/team_wnba_ny.json"), w));
  CHECK_EQ(w.event_id, std::string("401918297"));
  CHECK_EQ(w.kickoff_epoch, parse_iso8601_z("2026-10-07T23:30Z"));
  CHECK_EQ(w.team_color, std::string("86cebc"));
  CHECK_EQ(w.team_record, std::string("26-18"));
}

static void test_nba_live() {
  // 8:27 in the 3rd, IND 74 MIN 73. IND is home.
  GameSnapshot g = hoops_event("fixtures/event_nba_live.json", kPacers);
  CHECK(g.valid);
  CHECK(g.sport == Sport::BASKETBALL);
  CHECK(g.state == GameState::IN);
  CHECK(g.team_home);
  CHECK_EQ(g.team_abbr, std::string("IND"));
  CHECK_EQ(g.opp_abbr, std::string("MIN"));
  CHECK_EQ(g.team_score, 74);
  CHECK_EQ(g.opp_score, 73);
  CHECK_EQ(g.period, 3);
  CHECK_EQ((int) g.nba.regulation, 4);
  CHECK(g.nba.phase == Phase::PLAY);
  CHECK_EQ(g.nba.series, std::string(""));
  CHECK_EQ((int) g.nba.team_wins, -1);
  CHECK_EQ(g.possession, 0);
  CHECK_EQ(g.team_timeouts, 0);
  CHECK_EQ(g.team_logo, std::string("https://a.espncdn.com/combiner/i?img=/i/teamlogos/nba/500-dark/scoreboard/ind.png&w=64&h=64"));
  CHECK_EQ(clock_text(g), std::string("8:27 3rd"));
  CHECK_EQ(basketball_series(g), std::string(""));
  const std::string play = "Jaden McDaniels makes 14-foot driving floating jump shot (Jonathan Kuminga assists)";
  CHECK_EQ(g.last_play, play);
  TickerOptions o;
  o.clock = false;
  CHECK_EQ(status_text(g, o, ""), play);
  TickerOptions clock = o;
  clock.clock = true;
  CHECK_EQ(status_text(g, clock, ""), "8:27 - 3rd | " + play);
  TickerOptions quiet = o;
  quiet.last_play = false;
  CHECK_EQ(status_text(g, quiet, ""), std::string("8:27 - 3rd"));

  // Under a minute ESPN sends tenths and no minutes.
  GameSnapshot c = hoops_event("fixtures/event_nba_clock.json", kThunder);
  CHECK_EQ(c.display_clock, std::string("12.3"));
  CHECK_EQ(clock_text(c), std::string("12.3 1st"));
  CHECK_EQ(status_text(c, o, ""), std::string("Brayden Burries makes 17-foot pullup jump shot"));

  // Between quarters: the clock row names the break, the ticker the records.
  GameSnapshot e = hoops_event("fixtures/event_nba_end.json", kThunder);
  CHECK(e.nba.phase == Phase::END_PERIOD);
  CHECK_EQ(clock_text(e), std::string("End 1st"));
  CHECK_EQ(status_text(e, o, ""), std::string("OKC 0-1 | MIL 0-1"));

  // Overtime, from the period number: OT is period 5 with four quarters.
  GameSnapshot ot = g;
  ot.period = 5;
  CHECK_EQ(clock_text(ot), std::string("8:27 OT"));
  ot.period = 6;
  CHECK_EQ(clock_text(ot), std::string("8:27 2OT"));
  ot.nba.phase = Phase::END_PERIOD;
  CHECK_EQ(clock_text(ot), std::string("End 2OT"));
  ot.period = 4;
  CHECK_EQ(clock_text(ot), std::string("End 4th"));
  ot.nba.phase = Phase::OTHER;
  ot.short_detail = "Delayed";
  CHECK_EQ(clock_text(ot), std::string("Delayed"));
}

static void test_wnba_live() {
  // A semifinal, ATL leads the series 1-0: 4.5 left in the 2nd.
  GameSnapshot g = hoops_event("fixtures/event_wnba_live.json", kLiberty);
  CHECK(g.state == GameState::IN);
  CHECK(!g.team_home);
  CHECK_EQ(g.team_abbr, std::string("NY"));
  CHECK_EQ(g.team_score, 54);
  CHECK_EQ(g.opp_score, 56);
  CHECK_EQ(clock_text(g), std::string("4.5 2nd"));
  CHECK_EQ(g.nba.series, std::string("ATL leads series 1-0"));
  CHECK_EQ((int) g.nba.team_wins, 0);
  CHECK_EQ((int) g.nba.opp_wins, 1);
  CHECK_EQ(basketball_series(g), std::string("ATL lead 1-0"));
  CHECK_EQ(g.team_logo, std::string("https://a.espncdn.com/combiner/i?img=/i/teamlogos/wnba/500-dark/ny.png&w=64&h=64"));
  TickerOptions o;
  o.clock = false;
  CHECK_EQ(status_text(g, o, ""), std::string("Rhyne Howard makes 23-foot three point jumper (Jordin Canada assists)"));
  GameSnapshot a = hoops_event("fixtures/event_wnba_live.json", kDream);
  CHECK_EQ((int) a.nba.team_wins, 1);
  CHECK_EQ(basketball_series(a), std::string("ATL lead 1-0"));

  GameSnapshot h = hoops_event("fixtures/event_wnba_half.json", kLiberty);
  CHECK(h.nba.phase == Phase::HALFTIME);
  CHECK_EQ(clock_text(h), std::string("Halftime"));
  CHECK_EQ(status_text(h, o, ""), std::string("ATL leads series 1-0"));

  // The game went to overtime: period 5, "20.8 - OT".
  GameSnapshot ot = hoops_event("fixtures/event_wnba_ot.json", kDream);
  CHECK_EQ(ot.period, 5);
  CHECK_EQ(clock_text(ot), std::string("20.8 OT"));
  CHECK_EQ(basketball_series(ot), std::string("ATL lead 1-0"));

  // ESPN had not counted this game in the series yet when it went final.
  GameSnapshot f = hoops_event("fixtures/event_wnba_final.json", kDream);
  CHECK(f.state == GameState::POST);
  CHECK_EQ(f.team_score, 101);
  CHECK_EQ(f.opp_score, 98);
  CHECK_EQ(status_text(f, o, ""), std::string("Final/OT | ATL leads series 1-0"));
  CHECK_EQ(decide_splash(ot, f, true).text, std::string("ATL WINS!"));
  CHECK_EQ(decide_splash(ot, f, true).color, (uint32_t) 0xe31837);
  GameSnapshot ny_ot = hoops_event("fixtures/event_wnba_ot.json", kLiberty);
  GameSnapshot ny_f = hoops_event("fixtures/event_wnba_final.json", kLiberty);
  CHECK_EQ(decide_splash(ny_ot, ny_f, true).text, std::string(""));
  CHECK_EQ(decide_splash(ny_ot, ny_f, true, true).text, std::string("ATL WINS!"));

  // The short series line fits the 12-character situation row.
  GameSnapshot s = g;
  s.nba.team_wins = s.nba.opp_wins = 2;
  CHECK_EQ(basketball_series(s), std::string("Series 2-2"));
  s.team_abbr = "UTAH";
  s.nba.team_wins = 3;
  CHECK_EQ(basketball_series(s), std::string("UTAH up 3-2"));
}

static void test_nba_pre_post() {
  GameSnapshot p = hoops_event("fixtures/event_nba_pre.json", kBlazers);
  CHECK(p.state == GameState::PRE);
  CHECK_EQ(p.odds, std::string("POR -14.5"));
  CHECK_EQ(p.over_under, std::string("216.5"));
  CHECK_EQ(p.tv, std::string("NBA TV"));
  TickerOptions o;
  CHECK_EQ(status_text(p, o, "Today 10:00 PM"), std::string("Today 10:00 PM | POR -14.5 | O/U 216.5 | NBA TV | Moda Center"));
  TickerOptions no_odds;
  no_odds.odds = false;
  CHECK_EQ(status_text(p, no_odds, ""), std::string("10/7 - 10:00 PM EDT | Moda Center"));
  CHECK_EQ(basketball_series(p), std::string(""));

  GameSnapshot f = hoops_event("fixtures/event_nba_final.json", kPacers);
  CHECK(f.state == GameState::POST);
  CHECK(f.team_winner);
  CHECK_EQ(f.team_score, 123);
  CHECK_EQ(f.opp_score, 112);
  CHECK_EQ(status_text(f, o, ""), std::string("Final | IND 0-0 | MIN 1-0"));
  f.short_detail = "Final/OT";
  CHECK_EQ(status_text(f, o, ""), std::string("Final/OT | IND 0-0 | MIN 1-0"));

  GameSnapshot x;
  CHECK(!parse_event_str(slurp("fixtures/event_nba_live.json"), Sport::BASKETBALL, kBlazers, x));  // not in it
  CHECK(!parse_event_str("{nope", Sport::BASKETBALL, kPacers, x));
}

static void test_nba_splash() {
  GameSnapshot prev = hoops_event("fixtures/event_nba_live.json", kPacers);
  auto after = [&](int us, int them, GameState st) {
    GameSnapshot c = prev;
    c.team_score += us;
    c.opp_score += them;
    c.state = st;
    return c;
  };
  // Baskets never splash, whoever scores.
  CHECK_EQ(decide_splash(prev, after(3, 0, GameState::IN), true).text, std::string(""));
  CHECK_EQ(decide_splash(prev, after(0, 2, GameState::IN), true).text, std::string(""));
  CHECK_EQ(decide_splash(prev, after(2, 0, GameState::IN), true, true).text, std::string(""));
  Splash win = decide_splash(prev, after(10, 0, GameState::POST), true);
  CHECK_EQ(win.text, std::string("IND WINS!"));
  CHECK_EQ(win.color, (uint32_t) 0x0c2340);
  CHECK_EQ(decide_splash(prev, after(0, 10, GameState::POST), true).text, std::string(""));
  CHECK_EQ(decide_splash(prev, after(0, 10, GameState::POST), true, true).text, std::string("MIN WINS!"));
  GameSnapshot fin = after(10, 0, GameState::POST);
  CHECK_EQ(decide_splash(fin, fin, true).text, std::string(""));
  // The recorded final after the recorded live poll: IND won 123-112.
  GameSnapshot final_doc = hoops_event("fixtures/event_nba_final.json", kPacers);
  CHECK_EQ(decide_splash(prev, final_doc, true).text, std::string("IND WINS!"));
  CHECK_EQ(decide_splash(hoops_event("fixtures/event_nba_live.json", kWolves),
                         hoops_event("fixtures/event_nba_final.json", kWolves), true)
               .text,
           std::string(""));
}

// Men's college basketball, recorded 2026-10-07 in the off season: last
// season's finals (ESPN serves only the final state of a past game) and the
// coming season's opener (see fixtures/README.md).
static const uint32_t kMichigan = 130, kUConn = 41, kPenn = 219;

static void test_mcbb_urls() {
  CHECK_EQ(team_url(League::MCBB, 130),
           std::string("https://site.api.espn.com/apis/site/v2/sports/basketball/mens-college-basketball/teams/130"));
  CHECK_EQ(event_url(League::MCBB, "401856600"),
           std::string("https://site.api.espn.com/apis/site/v2/sports/basketball/mens-college-basketball/scoreboard/401856600"));
  CHECK_EQ(team_logo_url(League::MCBB, 130, "MICH"),
           std::string("https://a.espncdn.com/combiner/i?img=/i/teamlogos/ncaa/500-dark/130.png&w=64&h=64"));
  const LeagueInfo *l = league_by_key("mcbb");
  CHECK(l != nullptr && l->per_event && !l->season_list && l->logo_by_id && l->sport == Sport::BASKETBALL);
  CHECK_EQ(std::string(league_path(League::MCBB)), std::string("mens-college-basketball"));
  int n = 0;
  for (size_t i = 0; i < kTeamCount; i++) {
    const Team &t = kTeams[i];
    if (t.league != League::MCBB)
      continue;
    n++;
    if (t.espn_id == kMichigan)
      CHECK_EQ(team_option(t), std::string("NCAAM: Michigan Wolverines"));
  }
  CHECK(n >= 350);
}

static void test_mcbb() {
  Schedule s;
  CHECK(parse_team_str(slurp("fixtures/team_mcbb_mich.json"), s));
  CHECK_EQ(s.event_id, std::string("401925733"));
  CHECK_EQ(s.kickoff_epoch, parse_iso8601_z("2026-11-02T23:00Z"));
  CHECK_EQ(s.team_color, std::string("00274c"));

  GameSnapshot p = hoops_event("fixtures/event_mcbb_pre.json", kMichigan);
  CHECK(p.state == GameState::PRE);
  CHECK_EQ((int) p.nba.regulation, 2);
  CHECK_EQ(p.opp_abbr, std::string("OAK"));
  CHECK_EQ(p.team_logo, std::string("https://a.espncdn.com/combiner/i?img=/i/teamlogos/ncaa/500-dark/130.png&w=64&h=64"));
  TickerOptions o;
  CHECK_EQ(status_text(p, o, "Nov 2 6:00 PM"), std::string("Nov 2 6:00 PM | BTN | Crisler Center"));

  // The national championship: two halves, no series.
  GameSnapshot f = hoops_event("fixtures/event_mcbb_final.json", kUConn);
  CHECK(f.state == GameState::POST);
  CHECK_EQ((int) f.nba.regulation, 2);
  CHECK_EQ(f.team_score, 63);
  CHECK_EQ(f.opp_score, 69);
  CHECK_EQ(status_text(f, o, ""), std::string("Final | CONN 34-6 | MICH 37-3"));
  CHECK_EQ(basketball_series(f), std::string(""));

  // Overtime is period 3 after two halves.
  GameSnapshot ot = hoops_event("fixtures/event_mcbb_ot.json", kPenn);
  CHECK_EQ(ot.period, 3);
  CHECK(ot.team_winner);
  CHECK_EQ(status_text(ot, o, "").rfind("Final/OT | PENN ", 0), (size_t) 0);

  // Past games come back final only, so the live clock is checked on a copy.
  GameSnapshot live = f;
  live.state = GameState::IN;
  live.nba.phase = Phase::PLAY;
  live.period = 1;
  live.display_clock = "12:34";
  CHECK_EQ(clock_text(live), std::string("12:34 1st"));
  live.period = 2;
  CHECK_EQ(clock_text(live), std::string("12:34 2nd"));
  live.period = 3;
  CHECK_EQ(clock_text(live), std::string("12:34 OT"));
  live.period = 4;
  CHECK_EQ(clock_text(live), std::string("12:34 2OT"));
  live.nba.phase = Phase::HALFTIME;
  live.period = 1;
  CHECK_EQ(clock_text(live), std::string("Halftime"));
  live.nba.phase = Phase::END_PERIOD;
  live.period = 2;
  CHECK_EQ(clock_text(live), std::string("End 2nd"));
}

// A real home run (TB @ NYY, 2026-10-07, Top 6th): ESPN types the play
// "Play Result", so the splash relies on the play text.
static void test_mlb_home_run() {
  const uint32_t kRays = 30, kYankees = 10;
  GameSnapshot before = mlb_event("fixtures/event_mlb_hr_before.json", kRays);
  GameSnapshot after = mlb_event("fixtures/event_mlb_hr.json", kRays);
  CHECK_EQ(after.mlb.play_type, std::string("Play Result"));
  CHECK(after.last_play.find("homered") != std::string::npos);
  CHECK_EQ(after.team_score - before.team_score, 2);
  CHECK_EQ(decide_splash(before, after, true).text, std::string("HOME RUN!"));
  GameSnapshot nyy_before = mlb_event("fixtures/event_mlb_hr_before.json", kYankees);
  GameSnapshot nyy_after = mlb_event("fixtures/event_mlb_hr.json", kYankees);
  CHECK_EQ(decide_splash(nyy_before, nyy_after, true).text, std::string("TB HOME RUN"));
}

// NHL single-event documents (see fixtures/README.md)
static const uint32_t kJets = 28, kAvalanche = 17, kPenguins = 16, kCapitals = 23, kOilers = 6, kHurricanes = 7;

static GameSnapshot nhl_event(const char *file, uint32_t team) {
  GameSnapshot s;
  CHECK(parse_event_str(slurp(file), Sport::HOCKEY, team, s));
  return s;
}

static void test_nhl_urls() {
  CHECK_EQ(team_url(League::NHL, 6), std::string("https://site.api.espn.com/apis/site/v2/sports/hockey/nhl/teams/6"));
  CHECK_EQ(event_url(League::NHL, "401891830"),
           std::string("https://site.api.espn.com/apis/site/v2/sports/hockey/nhl/scoreboard/401891830"));
  CHECK_EQ(team_logo_url(League::NHL, 129764, "UTAH"),
           std::string("https://a.espncdn.com/combiner/i?img=/i/teamlogos/nhl/500-dark/utah.png&w=64&h=64"));
  CHECK_EQ(std::string(league_key(League::NHL)), std::string("nhl"));
  const LeagueInfo *nhl = league_by_key("nhl");
  CHECK(nhl != nullptr && nhl->per_event && !nhl->season_list && nhl->linger_min == 30);
  CHECK(league_sport(League::NHL) == Sport::HOCKEY);
  int count = 0;
  for (size_t i = 0; i < kTeamCount; i++) {
    if (kTeams[i].league != League::NHL)
      continue;
    count++;
    if (kTeams[i].espn_id == kPenguins)
      CHECK_EQ(team_option(kTeams[i]), std::string("NHL: Pittsburgh Penguins"));
  }
  CHECK_EQ(count, 32);
}

static void test_nhl_team() {
  Schedule s;
  CHECK(parse_team_str(slurp("fixtures/team_nhl_edm.json"), s));
  CHECK_EQ(s.event_id, std::string("401892456"));
  CHECK_EQ(s.kickoff_epoch, parse_iso8601_z("2026-10-08T02:00Z"));
  CHECK_EQ(s.team_color, std::string("00205b"));
  CHECK_EQ(s.team_record, std::string("2-0-1"));  // W-L-OTL
}

static void test_nhl_live() {
  // COL @ WPG, 3:21 left in the 1st, a delay of game penalty the last play
  GameSnapshot g = nhl_event("fixtures/event_nhl_live.json", kJets);
  CHECK(g.valid);
  CHECK(g.sport == Sport::HOCKEY);
  CHECK(g.state == GameState::IN);
  CHECK(g.team_home);
  CHECK(!g.nhl.playoffs);
  CHECK_EQ(g.nhl.series, std::string(""));
  CHECK_EQ(g.team_abbr, std::string("WPG"));
  CHECK_EQ(g.opp_abbr, std::string("COL"));
  CHECK_EQ(g.team_score, 0);
  CHECK_EQ(g.opp_score, 0);
  CHECK_EQ(g.period, 1);
  CHECK_EQ(g.possession, 0);
  CHECK_EQ(g.team_timeouts, 0);
  CHECK_EQ(g.down_distance, std::string(""));
  CHECK_EQ(g.team_record, std::string("2-0-1"));
  CHECK_EQ(g.opp_record, std::string("2-0-0"));
  CHECK_EQ(g.team_logo, std::string("https://a.espncdn.com/combiner/i?img=/i/teamlogos/nhl/500-dark/scoreboard/wpg.png&w=64&h=64"));
  CHECK_EQ(clock_text(g), std::string("3:21 1st"));
  const std::string penalty = "Delaying Game - Puck over glass served by Fedor Svechkov";
  CHECK_EQ(g.last_play, penalty);
  TickerOptions o;
  o.clock = false;
  CHECK_EQ(status_text(g, o, ""), penalty);
  TickerOptions with_clock;
  CHECK_EQ(status_text(g, with_clock, ""), "3:21 - 1st | " + penalty);
  TickerOptions quiet = o;
  quiet.last_play = false;
  CHECK_EQ(status_text(g, quiet, ""), std::string("3:21 - 1st"));
  GameSnapshot nameless = g;  // seen live: a goal before ESPN had the scorer
  nameless.last_play = ", assists: Neal Pionk (1), Dylan Samberg (1)";
  CHECK_EQ(status_text(nameless, o, ""), std::string("assists: Neal Pionk (1), Dylan Samberg (1)"));

  // The same game from the visitors' side
  GameSnapshot c = nhl_event("fixtures/event_nhl_live.json", kAvalanche);
  CHECK(!c.team_home);
  CHECK_EQ(c.team_abbr, std::string("COL"));
  CHECK_EQ(c.team_record, std::string("2-0-0"));

  // Intermission: PIT @ WSH after the 1st, the ticker shows the records
  GameSnapshot e = nhl_event("fixtures/event_nhl_end.json", kPenguins);
  CHECK(e.state == GameState::IN);
  CHECK(!e.team_home);
  CHECK_EQ(e.team_score, 1);
  CHECK_EQ(e.opp_score, 2);
  CHECK_EQ(clock_text(e), std::string("End of 1st"));
  CHECK_EQ(status_text(e, o, ""), std::string("PIT 2-1-0 | WSH 1-1-0"));
  CHECK_EQ(status_text(e, quiet, ""), std::string("PIT 2-1-0 | WSH 1-1-0"));
  GameSnapshot w = nhl_event("fixtures/event_nhl_end.json", kCapitals);
  CHECK_EQ(w.team_score, 2);
  CHECK_EQ(status_text(w, o, ""), std::string("WSH 1-1-0 | PIT 2-1-0"));

  // Overtime, then the shootout (regular season) or more overtime (playoffs)
  GameSnapshot ot = g;
  ot.period = 4;
  ot.display_clock = "3:10";
  ot.short_detail = "3:10 - OT";
  CHECK_EQ(clock_text(ot), std::string("3:10 OT"));
  ot.display_clock = "0:00";
  ot.short_detail = "End of OT";
  CHECK_EQ(clock_text(ot), std::string("End of OT"));
  GameSnapshot so = ot;
  so.period = 5;
  so.short_detail = "Shootout";
  CHECK_EQ(clock_text(so), std::string("SO"));
  so.short_detail = "0:00 - SO";
  CHECK_EQ(clock_text(so), std::string("SO"));
  GameSnapshot ot2 = so;
  ot2.nhl.playoffs = true;
  ot2.display_clock = "12:00";
  ot2.short_detail = "12:00 - 2OT";
  CHECK_EQ(clock_text(ot2), std::string("12:00 2OT"));
  ot2.period = 6;
  CHECK_EQ(clock_text(ot2), std::string("12:00 3OT"));
}

static void test_nhl_pre_post() {
  GameSnapshot p = nhl_event("fixtures/event_nhl_pre.json", kOilers);
  CHECK(p.state == GameState::PRE);
  CHECK(!p.team_home);
  CHECK_EQ(p.odds, std::string("EDM -135"));
  CHECK_EQ(p.over_under, std::string("6.5"));
  CHECK_EQ(p.tv, std::string("ESPN+"));
  TickerOptions o;
  CHECK_EQ(status_text(p, o, "Today 10:00 PM"), std::string("Today 10:00 PM | EDM -135 | O/U 6.5 | ESPN+ | Honda Center"));
  TickerOptions no_odds;
  no_odds.odds = false;
  CHECK_EQ(status_text(p, no_odds, ""), std::string("10/7 - 10:00 PM EDT | Honda Center"));

  // Tonight's final, and the win splash from the intermission document of the same game
  GameSnapshot f = nhl_event("fixtures/event_nhl_final.json", kCapitals);
  CHECK(f.state == GameState::POST);
  CHECK(f.team_winner);
  CHECK_EQ(f.team_score, 5);
  CHECK_EQ(f.opp_score, 3);
  CHECK_EQ(status_text(f, o, ""), std::string("Final | WSH 2-1-0 | PIT 2-2-0"));
  GameSnapshot w = nhl_event("fixtures/event_nhl_end.json", kCapitals);
  CHECK_EQ(decide_splash(w, f, true).text, std::string("WSH WINS!"));
  CHECK_EQ(decide_splash(w, f, true).color, (uint32_t) 0xd71830);
  GameSnapshot lost_prev = nhl_event("fixtures/event_nhl_end.json", kPenguins);
  GameSnapshot lost = nhl_event("fixtures/event_nhl_final.json", kPenguins);
  CHECK_EQ(decide_splash(lost_prev, lost, true).text, std::string(""));
  CHECK_EQ(decide_splash(lost_prev, lost, true, true).text, std::string("WSH WINS!"));

  // A regular-season shootout: CAR won it 5-4
  GameSnapshot so = nhl_event("fixtures/event_nhl_final_so.json", kPenguins);
  CHECK(so.state == GameState::POST);
  CHECK(!so.team_winner);
  CHECK_EQ(so.period, 5);
  CHECK_EQ(so.team_score, 4);
  CHECK_EQ(so.opp_score, 5);
  CHECK_EQ(clock_text(so), std::string("Final/SO"));
  CHECK_EQ(status_text(so, o, ""), std::string("Final/SO | PIT 32-17-15 | CAR 41-17-6"));

  // A playoff game in double overtime: the series line stands in for records
  GameSnapshot po = nhl_event("fixtures/event_nhl_final_2ot.json", kHurricanes);
  CHECK(po.nhl.playoffs);
  CHECK(po.team_winner);
  CHECK_EQ(po.nhl.series, std::string("CAR leads series 2-0"));
  CHECK_EQ(status_text(po, o, ""), std::string("Final/2OT | CAR leads series 2-0"));
  GameSnapshot pre = po;
  pre.state = GameState::PRE;
  pre.odds = pre.over_under = pre.tv = "";
  CHECK_EQ(status_text(pre, o, "Today 7:00 PM"), std::string("Today 7:00 PM | CAR leads series 2-0 | Lenovo Center"));

  GameSnapshot x;
  CHECK(!parse_event_str(slurp("fixtures/event_nhl_live.json"), Sport::HOCKEY, kPenguins, x));  // not in it
  CHECK(!parse_event_str("{nope", Sport::HOCKEY, kJets, x));
}

static void test_nhl_splash() {
  GameSnapshot prev = nhl_event("fixtures/event_nhl_live.json", kJets);
  auto after = [&](int us, int them) {
    GameSnapshot c = prev;
    c.team_score += us;
    c.opp_score += them;
    return c;
  };
  CHECK_EQ(decide_splash(prev, after(1, 0), true).text, std::string("GOAL!"));
  CHECK_EQ(decide_splash(prev, after(1, 0), false).color, (uint32_t) 0x002d62);
  CHECK_EQ(decide_splash(prev, after(0, 1), true).text, std::string("COL GOAL"));
  CHECK_EQ(decide_splash(prev, after(0, 1), true).color, (uint32_t) 0x860038);
  CHECK_EQ(decide_splash(prev, after(0, 1), false).text, std::string(""));
  CHECK_EQ(decide_splash(prev, after(0, 0), true).text, std::string(""));
  CHECK_EQ(decide_splash(after(1, 0), prev, true).text, std::string(""));  // goal taken back on review
  // Live modes: neither side is ours
  CHECK_EQ(decide_splash(prev, after(1, 0), true, true).text, std::string("WPG GOAL"));
  CHECK_EQ(decide_splash(prev, after(0, 1), true, true).text, std::string("COL GOAL"));
  // The win, including a shootout decided between two polls
  GameSnapshot fin = after(1, 0);
  fin.state = GameState::POST;
  CHECK_EQ(decide_splash(prev, fin, true).text, std::string("WPG WINS!"));
  GameSnapshot lost = after(0, 1);
  lost.state = GameState::POST;
  CHECK_EQ(decide_splash(prev, lost, true).text, std::string(""));
  CHECK_EQ(decide_splash(prev, lost, true, true).text, std::string("COL WINS!"));
  GameSnapshot other = after(1, 0);
  other.event_id = "1";
  CHECK_EQ(decide_splash(prev, other, true).text, std::string(""));
  GameSnapshot pre = prev;
  pre.state = GameState::PRE;
  CHECK_EQ(decide_splash(pre, after(1, 0), true).text, std::string(""));
}

// ESPN's team endpoint keeps naming a finished game for hours (PIT's still
// named the 10/7 final at 11 AM Eastern on 10/8). Leagues without a season
// list find the next game in the coming days' scoreboards.
static void test_lookahead() {
  Schedule pit;
  CHECK(parse_team_str(slurp("fixtures/team_nhl_pit_post.json"), pit));
  CHECK_EQ(pit.event_id, std::string("401891830"));
  CHECK(pit.next_final);
  Schedule atl;
  CHECK(parse_team_str(slurp("fixtures/team_mlb_atl.json"), atl));
  CHECK(!atl.next_final);  // its next event was in progress
  Schedule dal;
  CHECK(parse_team_str(slurp("fixtures/team_nfl_dal.json"), dal));
  CHECK(!dal.next_final);

  Schedule next;
  CHECK(parse_team_game_str(slurp("fixtures/day_nhl_20261009.json"), 16, next));
  CHECK_EQ(next.event_id, std::string("401892467"));  // PIT @ CBJ
  CHECK_EQ(next.kickoff_epoch, parse_iso8601_z("2026-10-09T23:00Z"));
  Schedule none;
  CHECK(!parse_team_game_str(slurp("fixtures/day_nhl_20261009.json"), 17, none));  // COL is off
  CHECK(!parse_team_game_str(slurp("fixtures/day_mlb_20261009.json"), 15, none));   // no games at all
  // Game 2 of a doubleheader, after game 1's final.
  const char *dh = R"({"events":[
    {"id":"1","date":"2026-07-04T17:05Z","status":{"type":{"state":"post"}},
     "competitions":[{"competitors":[{"id":"15"},{"id":"19"}]}]},
    {"id":"2","date":"2026-07-04T22:10Z","status":{"type":{"state":"pre"}},
     "competitions":[{"competitors":[{"id":"19"},{"id":"15"}]}]}]})";
  CHECK(parse_team_game_str(dh, 15, next));
  CHECK_EQ(next.event_id, std::string("2"));
  // The scoreboard can still call game 1 live after the panel saw it end.
  const char *dh_live = R"({"events":[
    {"id":"1","date":"2026-07-04T17:05Z","status":{"type":{"state":"in"}},
     "competitions":[{"competitors":[{"id":"15"},{"id":"19"}]}]},
    {"id":"2","date":"2026-07-04T22:10Z","status":{"type":{"state":"pre"}},
     "competitions":[{"competitors":[{"id":"19"},{"id":"15"}]}]}]})";
  CHECK(parse_team_game_str(dh_live, 15, next, "1"));
  CHECK_EQ(next.event_id, std::string("2"));
  CHECK(!parse_team_game_str(slurp("fixtures/day_nhl_20261009.json"), 16, none, "401892467"));
  // A document that does not parse is a failed read, not an off day.
  std::string bad = "{\"events\":[";
  CHECK_EQ(parse_team_game(bad, 16, std::string(), none), -1);
  std::string off = slurp("fixtures/day_mlb_20261009.json");
  CHECK_EQ(parse_team_game(off, 15, std::string(), none), 0);

  CHECK_EQ(team_day_url(League::NHL, 0, parse_iso8601_z("2026-10-09T15:00Z")),
           std::string("https://site.api.espn.com/apis/site/v2/sports/hockey/nhl/scoreboard?dates=20261009"));
  CHECK_EQ(team_day_url(League::MCBB, 8, parse_iso8601_z("2026-12-06T18:00Z")),
           std::string("https://site.api.espn.com/apis/site/v2/sports/basketball/mens-college-basketball/scoreboard?groups=8&dates=20261206"));
}

int run_football_golden();  // golden_football.cpp

static void test_logo_lru() {
  using espn::LogoSlot;
  // Oldest idle, off-screen slot goes first.
  CHECK_EQ(espn::pick_evict({{300, false, false}, {100, false, false}, {200, false, false}}), 1);
  // On screen or still downloading: never dropped, even when oldest.
  CHECK_EQ(espn::pick_evict({{100, false, true}, {150, true, false}, {200, false, false}}), 2);
  // Nothing droppable.
  CHECK_EQ(espn::pick_evict({{100, true, false}, {200, false, true}}), -1);
  CHECK_EQ(espn::pick_evict({}), -1);
  // Retry after 15 s, including across the millis() wrap.
  CHECK(!espn::logo_retry_due(1000, 1000 + espn::LOGO_RETRY_MS - 1));
  CHECK(espn::logo_retry_due(1000, 1000 + espn::LOGO_RETRY_MS));
  CHECK(espn::logo_retry_due(0xFFFFF000u, 0xFFFFF000u + espn::LOGO_RETRY_MS));
}

static void test_timezone_index() {
  // Names first: the app sends one, so a reordered list can't pick a wrong zone.
  CHECK_EQ(espn::timezone_index("US Central"), 1);
  CHECK_EQ(espn::timezone_index("America/Chicago"), 1);
  CHECK_EQ(espn::timezone_index("Australia Eastern"), 12);
  CHECK_EQ(espn::timezone_index("Australia/Sydney"), 12);
  CHECK_EQ(espn::timezone_index("UTC"), 13);
  // Positions still work for the device page, which is built from this list.
  CHECK_EQ(espn::timezone_index("0"), 0);
  CHECK_EQ(espn::timezone_index("13"), 13);
  CHECK_EQ(espn::timezone_index("14"), -1);
  CHECK_EQ(espn::timezone_index("99999999999999999999"), -1);
  // Nothing close enough to guess at.
  CHECK_EQ(espn::timezone_index("us central"), -1);
  CHECK_EQ(espn::timezone_index("America/Detroit"), -1);
  CHECK_EQ(espn::timezone_index("-1"), -1);
  CHECK_EQ(espn::timezone_index("1a"), -1);
  CHECK_EQ(espn::timezone_index(""), -1);
  CHECK_EQ(espn::timezone_index(nullptr), -1);
  // Every row answers to its own name and IANA name.
  for (size_t i = 0; i < espn::kTimezoneCount; i++) {
    CHECK_EQ(espn::timezone_index(espn::kTimezones[i].name), (int) i);
    CHECK_EQ(espn::timezone_index(espn::kTimezones[i].iana), (int) i);
  }
}

// Recorded 2026-10-08 from the 2025-26 postseason, see fixtures/README.md
static void test_postseason() {
  TickerOptions all;
  // CFP quarterfinal at the Rose Bowl, a neutral site, from both sides
  std::string qf = slurp("fixtures/scoreboard_ncaa_cfp_quarterfinals.json");
  GameSnapshot ala;
  CHECK(parse_scoreboard_str(qf, "401769072", 333, ala));
  CHECK(ala.state == GameState::POST);
  CHECK(ala.completed);
  CHECK_EQ(ala.team_score, 3);
  CHECK_EQ(ala.opp_score, 38);
  CHECK(!ala.team_winner);
  CHECK_EQ(ala.venue, std::string("Rose Bowl"));
  CHECK_EQ(status_text(ala, all, ""), std::string("Final | ALA 11-4 | IU 14-0"));
  GameSnapshot iu;
  CHECK(parse_scoreboard_str(qf, "401769072", 84, iu));
  CHECK(iu.team_winner);
  CHECK_EQ(iu.opp_abbr, std::string("ALA"));
  // The My team poll filters by our conference. ESPN lists a game under
  // both teams' conferences, so SEC (8) finds Alabama's Rose Bowl.
  CHECK_EQ(scoreboard_url(League::NCAA, 8, ala.kickoff_epoch),
           std::string("https://site.api.espn.com/apis/site/v2/sports/football/college-football/scoreboard"
                       "?groups=8&dates=20260101"));

  // National championship: 7:30 PM Eastern is 00:30 UTC the next day
  std::string final_json = slurp("fixtures/scoreboard_ncaa_cfp_final.json");
  GameSnapshot mia;
  CHECK(parse_scoreboard_str(final_json, "401769076", 2390, mia));
  CHECK(mia.completed);
  CHECK_EQ(mia.team_score, 21);
  CHECK_EQ(mia.opp_score, 27);
  CHECK(!mia.team_winner);
  CHECK(scoreboard_url(League::NCAA, 1, mia.kickoff_epoch).find("&dates=20260119") != std::string::npos);

  // Conference title game that went to overtime
  GameSnapshot duke;
  CHECK(parse_scoreboard_str(slurp("fixtures/scoreboard_ncaa_conf_titles.json"), "401777328", 150, duke));
  CHECK(duke.completed);
  CHECK(duke.team_winner);
  CHECK_EQ(duke.short_detail, std::string("Final/OT"));
  CHECK_EQ(status_text(duke, all, ""), std::string("Final | DUKE 8-5 | UVA 10-3"));

  // NFL wild card and the Super Bowl
  GameSnapshot chi;
  CHECK(parse_scoreboard_str(slurp("fixtures/scoreboard_nfl_wild_card.json"), "401772981", 3, chi));
  CHECK(chi.completed);
  CHECK(chi.team_winner);
  CHECK_EQ(chi.team_score, 31);
  CHECK_EQ(chi.opp_score, 27);
  GameSnapshot sea;
  CHECK(parse_scoreboard_str(slurp("fixtures/scoreboard_nfl_super_bowl.json"), "401772988", 26, sea));
  CHECK(sea.completed);
  CHECK(sea.team_winner);
  CHECK_EQ(sea.opp_abbr, std::string("NE"));
  CHECK_EQ(sea.venue, std::string("Levi's Stadium"));
  CHECK(scoreboard_url(League::NFL, 0, sea.kickoff_epoch).find("?dates=20260208") != std::string::npos);
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
  test_upcoming();
  test_clock_text();
  test_color();
  test_favorites();
  test_schedule_league();
  test_schedule_due();
  test_team_cache();
  test_boot_hold();
  test_rotate_minutes();
  test_mlb_urls();
  test_mlb_team();
  test_mlb_live();
  test_mlb_pre_post();
  test_mlb_splash();
  test_mlb_home_run();
  test_favorites_filters();
  test_soccer_urls();
  test_soccer_team();
  test_soccer_pre();
  test_soccer_live();
  test_soccer_finals();
  test_soccer_splash();
  test_soccer_text_helpers();
  test_soccer_cups();
  test_lookahead();
  test_nhl_urls();
  test_nhl_team();
  test_nhl_live();
  test_nhl_pre_post();
  test_nhl_splash();
  test_nba_urls();
  test_nba_team();
  test_nba_live();
  test_wnba_live();
  test_nba_pre_post();
  test_nba_splash();
  test_mcbb_urls();
  test_mcbb();
  checks++;
  failures += run_football_golden();
  test_logo_lru();
  test_timezone_index();
  test_postseason();
  printf("%d checks, %d failures\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
