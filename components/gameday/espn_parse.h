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

enum class Half : uint8_t { NONE, TOP, MID, BOTTOM, END };

// Baseball's live state. Zero or -1 when the document has none (pre-game,
// finals, between half innings).
struct Baseball {
  static constexpr uint8_t FIRST = 1, SECOND = 2, THIRD = 4;
  int8_t balls{-1}, strikes{-1}, outs{-1};
  uint8_t bases{0};  // FIRST | SECOND | THIRD
  Half half{Half::NONE};
  int team_hits{0}, opp_hits{0}, team_errors{0}, opp_errors{0};
  std::string batter, pitcher;               // "A. Riley", while a half inning is on
  std::string team_probable, opp_probable;   // starting pitchers, pre-game
  std::string series;                        // "LAD lead series 2-1", postseason
  std::string play_type;                     // last play's type: "Home Run", "Ball"
};

// Where a basketball game is, from ESPN's status name.
enum class Phase : uint8_t { OTHER, PLAY, END_PERIOD, HALFTIME };

// Basketball (NBA, WNBA, men's college). The series fields are set only for
// a playoff game.
struct Basketball {
  uint8_t regulation{4};  // periods before overtime: 4 quarters, or 2 halves in college
  Phase phase{Phase::OTHER};
  std::string series;                  // ESPN's line: "ATL leads series 1-0"
  int8_t team_wins{-1}, opp_wins{-1};  // series wins so far, -1 outside a series
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
  // Other sports. Football leaves these at their defaults.
  Sport sport{Sport::FOOTBALL};
  bool team_home{false};
  Baseball mlb;
  Basketball nba;  // every basketball league
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

// A game found by scanning a day's scoreboard (live-game modes).
struct LiveGame {
  uint8_t league{0};
  std::string event_id;
  uint32_t group{0};        // home team's conference, for the follow-up polls
  uint32_t away_id{0};      // shown on the left, like a broadcast
  std::string away_abbr, home_abbr;
};

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

const char *league_path(League league);  // the last part of the ESPN path: "nfl"
std::string team_option(const Team &t);  // "NFL: Dallas Cowboys", the Home Assistant option
std::string team_url(League league, uint32_t espn_id);
std::string schedule_url(League league, uint32_t espn_id);  // the season's games, ~200KB
std::string scoreboard_url(League league, uint32_t group, int64_t kickoff_epoch);
std::string scan_url(League league, int64_t now_epoch);  // every game of the day for one league
// One game on its own (8-18 KB). Leagues with LeagueInfo::per_event poll this
// instead of the day's whole scoreboard; football does not use it.
std::string event_url(League league, const std::string &event_id);
std::string dark_logo(const std::string &url);
std::string team_logo_url(League league, uint32_t espn_id, const char *abbr);  // for a team with no game loaded yet

bool parse_team_str(const std::string &json, Schedule &out);
// Up to `max` games not yet started, in date order, from the schedule endpoint.
bool parse_upcoming_str(const std::string &json, uint32_t our_team_id, size_t max, std::vector<Upcoming> &out);
bool parse_scoreboard_str(const std::string &json, const std::string &event_id, uint32_t our_team_id,
                          GameSnapshot &out);
// The single-event document from event_url().
bool parse_event_str(const std::string &json, Sport sport, uint32_t our_team_id, GameSnapshot &out);

// neutral = nobody is "our" team (live-game modes): both sides splash with their abbreviation.
Splash decide_splash(const GameSnapshot &prev, const GameSnapshot &cur, bool opponent_splashes, bool neutral = false);
uint32_t parse_color(const std::string &hex);
std::string status_text(const GameSnapshot &s, const TickerOptions &o, const std::string &kickoff_local);
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

// Per-sport text and splashes. status_text, clock_text and decide_splash hand
// any snapshot that is not football to these (sport_baseball.cpp).
std::string baseball_status_text(const GameSnapshot &s, const TickerOptions &o, const std::string &kickoff_local);
std::string baseball_clock_text(const GameSnapshot &s);
Splash baseball_splash(const GameSnapshot &prev, const GameSnapshot &cur, bool opponent_splashes, bool neutral);
// The count while a half inning is on ("2-1"), else "".
std::string baseball_count(const GameSnapshot &s);

// Basketball (sport_basketball.cpp). The JSON filter and extras parse are
// declared in espn_parse_impl.h, next to ArduinoJson.
std::string basketball_status_text(const GameSnapshot &s, const TickerOptions &o, const std::string &kickoff_local);
std::string basketball_clock_text(const GameSnapshot &s);
Splash basketball_splash(const GameSnapshot &prev, const GameSnapshot &cur, bool opponent_splashes, bool neutral);
// The playoff series in at most 12 characters for the situation row
// ("NY lead 2-1", "Series 2-2"), else "".
std::string basketball_series(const GameSnapshot &s);

namespace detail {
// Ticker helpers shared by the sport files.
void add_part(std::string &out, const std::string &part);
std::string records_line(const GameSnapshot &s);
}  // namespace detail

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
