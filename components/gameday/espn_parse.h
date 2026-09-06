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
std::string dark_logo(const std::string &url);

bool parse_team_str(const std::string &json, Schedule &out);
bool parse_scoreboard_str(const std::string &json, const std::string &event_id, uint32_t our_team_id,
                          GameSnapshot &out);

Splash decide_splash(const GameSnapshot &prev, const GameSnapshot &cur, bool opponent_splashes);
uint32_t parse_color(const std::string &hex);
std::string status_text(const GameSnapshot &s, const TickerOptions &o, const std::string &kickoff_local);
std::string kickoff_label(const struct tm &kick_local, const struct tm &now_local);
int64_t parse_iso8601_z(const std::string &s);
const char *state_name(GameState s);

}  // namespace espn

// Streaming entry points live in the header because they are templates on
// the reader type (a std::string on the host, an HTTP container on device).
#include "espn_parse_impl.h"
