// Pure ESPN parsing and game logic. No ESPHome includes so the host test
// binary can compile this file directly.
#pragma once

#include <cstdint>
#include <ctime>
#include <string>
#include <vector>

#include "teams.h"

namespace espn {

enum class GameState : uint8_t { NOT_FOUND, PRE, IN, POST };

// What the team endpoint tells us about the next game.
struct Schedule {
  bool valid{false};
  uint8_t league{0};  // League as uint8_t; set for live-mode games
  std::string event_id;
  int64_t kickoff_epoch{0};
  uint32_t group{0};
  std::string team_color;
  std::string team_record;
};

// One scoreboard event, resolved to "us" and "them".
struct GameSnapshot {
  bool valid{false};
  GameState state{GameState::NOT_FOUND};
  std::string event_id;
  int64_t kickoff_epoch{0};
  bool completed{false};
  std::string short_detail;
  std::string display_clock;
  int period{0};
  std::string team_abbr, opp_abbr;
  uint32_t team_id{0}, opp_id{0};
  int team_score{0}, opp_score{0};
  std::string team_record, opp_record;
  std::string team_color, opp_color;
  std::string team_logo, opp_logo;
  bool team_winner{false};
  int possession{0};
  int team_timeouts{0}, opp_timeouts{0};
  std::string last_play, down_distance;
  std::string short_down_distance;  // "3rd & 10"
  bool is_red_zone{false};
  std::string odds, over_under, tv, venue;
};

// One future game from the team's schedule endpoint, for the "Up next" list.
struct Upcoming {
  std::string event_id;
  int64_t kickoff_epoch{0};
  uint32_t opp_id{0};
  std::string opp_abbr, opp_name;
  bool home{false};     // we are the home side
  bool neutral{false};  // neutral site
  std::string tv;
};

// The team's last finished game, for the idle Record screen.
struct GameResult {
  bool valid{false};
  int64_t kickoff_epoch{0};
  std::string opp_abbr;
  int us{0}, them{0};
  bool home{false};
  bool neutral{false};
};

// Everything the idle screens need about the saved team, from the one
// season-schedule download (no team endpoint needed).
struct TeamSchedule {
  bool valid{false};
  std::string color, record, standing;  // "002a5c", "2-1", "1st in NFC East"
  uint32_t group{0};                    // division (NFL) or conference (college)
  std::vector<Upcoming> upcoming;
  GameResult last;
};

struct StandingRow {
  uint32_t team_id{0};
  std::string abbr, record;
};

// One division or conference, in ESPN's order (which is the standings order).
struct Standings {
  bool valid{false};
  std::string title;  // "NFC East", "ACC"
  std::vector<StandingRow> rows;
};

// A game found by scanning a day's scoreboard (live-game modes).
struct LiveGame {
  uint8_t league{0};
  std::string event_id;
  GameState state{GameState::NOT_FOUND};
  int64_t kickoff_epoch{0};
  uint32_t group{0};        // home team's conference, for the follow-up polls
  uint32_t away_id{0};      // shown on the left, like a broadcast
  uint32_t home_id{0};
  std::string away_abbr, home_abbr;
};

// What one league's scoreboard says for the live modes: the games in
// progress, the finals, and the earliest game that has not started.
struct ScanResult {
  std::vector<LiveGame> live;
  std::vector<LiveGame> finals;
  LiveGame later;           // valid when later.state == PRE
  int week{0};              // the scoreboard's "week.number", 0 when absent
  size_t events{0};         // events read
  bool stopped{false};      // read stopped early (see parse_scan)
};

// parse_scan stops at the first unstarted game kicking off this long after
// `now`. Anything before it that has started is either in progress or final.
static const int64_t SCAN_STOP_MARGIN = 30 * 60;

// Live modes with nothing in progress: what the board shows instead, in
// order of precedence (see live_precedence).
enum class LiveShow : uint8_t {
  NONE,         // no scan has landed yet
  LIVE,         // following a game in progress
  LATER_TODAY,  // the league's next kickoff today
  FINAL_TODAY,  // today's finals, one at a time
  NEXT_GAME,    // the league's next game this week
  FETCH_NEXT,   // the next game is not known yet: fetch it first
  IDLE,         // nothing at all (off season)
  MY_TEAM,      // the owner chose to see their own team instead
};

// Next game this week, per league: unknown until fetched, then found or not.
enum class NextState : uint8_t { UNKNOWN, NONE, FOUND };

struct LiveInputs {
  bool any_live{false};
  bool later_today{false};
  size_t finals_today{0};
  NextState next{NextState::UNKNOWN};
  bool my_team_fallback{false};  // the "my_team" setting, with a saved team
};

inline LiveShow live_precedence(const LiveInputs &in) {
  if (in.any_live)
    return LiveShow::LIVE;
  if (in.my_team_fallback)
    return LiveShow::MY_TEAM;
  if (in.later_today)
    return LiveShow::LATER_TODAY;
  if (in.finals_today > 0)
    return LiveShow::FINAL_TODAY;
  switch (in.next) {
    case NextState::FOUND:
      return LiveShow::NEXT_GAME;
    case NextState::UNKNOWN:
      return LiveShow::FETCH_NEXT;
    default:
      return LiveShow::IDLE;
  }
}

// Day number in local time, for "until midnight" tests. offset_s is the
// local UTC offset in seconds (negative west of Greenwich).
inline int64_t local_day(int64_t epoch, int32_t offset_s) {
  int64_t t = epoch + offset_s;
  return t >= 0 ? t / 86400 : (t - 86399) / 86400;
}

// Keeps the finals that kicked off on the same local day as `now`.
inline std::vector<LiveGame> finals_today(const std::vector<LiveGame> &finals, int64_t now, int32_t offset_s) {
  std::vector<LiveGame> out;
  int64_t today = local_day(now, offset_s);
  for (const auto &g : finals)
    if (g.kickoff_epoch != 0 && local_day(g.kickoff_epoch, offset_s) == today)
      out.push_back(g);
  return out;
}

struct TickerOptions {
  bool clock{true};
  bool down_distance{true};
  bool last_play{true};
  bool odds{true};
};

struct Splash {
  std::string text;
  uint32_t color{0};
};

const char *league_path(League league);
std::string team_url(League league, uint32_t espn_id);
std::string schedule_url(League league, uint32_t espn_id);  // the season's games, ~200KB
std::string scoreboard_url(League league, uint32_t group, int64_t kickoff_epoch);
// Every game of the owner's local day for one league. utc_offset_s is the
// local offset (ESPTime::timezone_offset()); ESPN keys a game day by date.
std::string scan_url(League league, int64_t now_epoch, int32_t utc_offset_s);
// The league's current week, or `week` when it is not 0 (next-game lookup).
std::string week_url(League league, int week);
std::string standings_url(League league, uint32_t group);
std::string dark_logo(const std::string &url);
std::string team_logo_url(League league, uint32_t espn_id, const char *abbr);  // for a team with no game loaded yet

bool parse_team_str(const std::string &json, Schedule &out);
// Up to `max` games not yet started, in date order, from the schedule endpoint.
bool parse_upcoming_str(const std::string &json, uint32_t our_team_id, size_t max, std::vector<Upcoming> &out);
bool parse_scoreboard_str(const std::string &json, const std::string &event_id, uint32_t our_team_id,
                          GameSnapshot &out);

// neutral = nobody is "our" team (live-game modes): both sides splash with their abbreviation.
Splash decide_splash(const GameSnapshot &prev, const GameSnapshot &cur, bool opponent_splashes, bool neutral = false);
uint32_t parse_color(const std::string &hex);
std::string status_text(const GameSnapshot &s, const TickerOptions &o, const std::string &kickoff_local);
// "W 37-20 vs WSH", "L 15-34 at CIN", "T 20-20 vs NYG"; empty when unknown.
std::string result_line(const GameResult &r);

// Ticker for a live mode's board. league_word is "college ", "NFL " or "".
// kickoff_local is kickoff_label()'s text; base is status_text()'s.
std::string live_ticker(LiveShow show, const std::string &league_word, const GameSnapshot &s, const TickerOptions &o,
                        const std::string &kickoff_local, const std::string &base);
std::string kickoff_label(const struct tm &kick_local, const struct tm &now_local);
int64_t parse_iso8601_z(const std::string &s);
const char *state_name(GameState s);
// "0:42 2nd" from the raw clock and period. ESPN's pre-formatted shortDetail
// lags those fields by a poll or two, so it is only used for the states that
// have no clock (Halftime, End of 3rd, Delayed).
std::string clock_text(const GameSnapshot &s);

// The team endpoint does not say which league answered, so a freshly parsed
// Schedule carries league 0 (NFL). Favorites and the live modes choose the
// follow-up scoreboard endpoint from Schedule::league, so it must be stamped
// from the team the schedule was fetched for.
inline void adopt_league(Schedule &s, League league) { s.league = (uint8_t) league; }

// Does this cycle need to re-read the team endpoint, or only re-poll the game
// it already knows about? `force` is the Refresh Now button. It has to be its
// own flag: clearing the fetch timestamp does not force anything, because a
// still valid schedule with a zero timestamp reads as "age = millis()", which
// is under the interval for the first six hours after a restart. Unsigned
// subtraction keeps the age right across the millis() wrap.
inline bool schedule_due(bool have_schedule, bool force, uint32_t now, uint32_t fetched_ms, uint32_t interval) {
  return force || !have_schedule || (now - fetched_ms) >= interval;
}

}  // namespace espn

// Streaming entry points live in the header because they are templates on
// the reader type (a std::string on the host, an HTTP container on device).
#include "espn_parse_impl.h"
