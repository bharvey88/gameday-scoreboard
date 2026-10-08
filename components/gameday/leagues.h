// Every league the panel can follow and what differs between them. Pure, no
// ESPHome includes, so the host tests use it too. scripts/build_teams.py,
// scripts/build_web.py and __init__.py read kLeagues with a regex: keep one
// row per line in the same field order.
#pragma once

#include <cstdint>
#include <cstring>

namespace espn {

// Saved in prefs and the boot cache: append, never renumber.
enum class League : uint8_t { NFL = 0, NCAA = 1, NBA = 2, NHL = 3, MLB = 4, WNBA = 5, MLS = 6, EPL = 7, MCBB = 8 };

enum class Sport : uint8_t { FOOTBALL = 0, BASEBALL = 1, BASKETBALL = 2, HOCKEY = 3, SOCCER = 4 };

struct LeagueInfo {
  League league;
  const char *key;       // team refs and the state document: "nfl", "mlb"
  const char *path;      // ESPN site API path: "football/nfl"
  const char *prefix;    // Home Assistant team options and logs: "NFL"
  const char *logo_dir;  // a.espncdn.com/i/teamlogos/<dir>/500/...
  Sport sport;
  bool per_event;      // poll scoreboard/<event id>, not the day's whole slate
  bool season_list;    // the team schedule is small enough for the Up next list
  bool logo_by_id;     // logo files are named by team id, not abbreviation
  uint8_t linger_min;  // a final stays on the board this long before the next game
};

constexpr LeagueInfo kLeagues[] = {
    {League::NFL, "nfl", "football/nfl", "NFL", "nfl", Sport::FOOTBALL, false, true, false, 30},
    {League::NCAA, "ncaa", "football/college-football", "NCAAF", "ncaa", Sport::FOOTBALL, false, true, true, 30},
    // A regular-season team schedule is ~2.7 MB, so no Up next list. Game 2
    // of a doubleheader can start half an hour after game 1: short linger.
    {League::MLB, "mlb", "baseball/mlb", "MLB", "mlb", Sport::BASEBALL, true, false, false, 10},
    // An 82-game NBA season schedule is ~1 MB (WNBA ~840 KB): no Up next list.
    {League::NBA, "nba", "basketball/nba", "NBA", "nba", Sport::BASKETBALL, true, false, false, 30},
    {League::WNBA, "wnba", "basketball/wnba", "WNBA", "wnba", Sport::BASKETBALL, true, false, false, 30},
    // A college season schedule is ~570 KB. Its teams are left out of Home
    // Assistant's team select (__init__.py).
    {League::MCBB, "mcbb", "basketball/mens-college-basketball", "NCAAM", "ncaa", Sport::BASKETBALL, true, false, true, 30},
};

constexpr size_t kLeagueCount = sizeof(kLeagues) / sizeof(kLeagues[0]);

inline const LeagueInfo *league_info(League league) {
  for (const auto &l : kLeagues)
    if (l.league == league)
      return &l;
  return nullptr;
}

inline const LeagueInfo *league_by_key(const char *key) {
  for (const auto &l : kLeagues)
    if (strcmp(l.key, key) == 0)
      return &l;
  return nullptr;
}

// "nfl" for a known league, "" for a byte this firmware does not know.
inline const char *league_key(League league) {
  const LeagueInfo *l = league_info(league);
  return l != nullptr ? l->key : "";
}

inline Sport league_sport(League league) {
  const LeagueInfo *l = league_info(league);
  return l != nullptr ? l->sport : Sport::FOOTBALL;
}

}  // namespace espn
