// Pure ESPN parsing and game logic. No ESPHome includes so the host test
// binary can compile this file directly.
#pragma once

#include <cstdint>
#include <ctime>
#include <string>

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

const char *league_path(League league);
std::string team_url(League league, uint32_t espn_id);
std::string scoreboard_url(League league, uint32_t group, int64_t kickoff_epoch);
std::string scan_url(League league, int64_t now_epoch);  // every game of the day for one league
std::string dark_logo(const std::string &url);
std::string team_logo_url(League league, uint32_t espn_id, const char *abbr);  // for a team with no game loaded yet

bool parse_team_str(const std::string &json, Schedule &out);
bool parse_scoreboard_str(const std::string &json, const std::string &event_id, uint32_t our_team_id,
                          GameSnapshot &out);

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

}  // namespace espn

// Streaming entry points live in the header because they are templates on
// the reader type (a std::string on the host, an HTTP container on device).
#include "espn_parse_impl.h"
