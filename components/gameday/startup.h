// Startup helpers: the saved team info a boot can start from, and how long
// the boot screen waits for the first scoreboard. Pure, with no ESPHome
// includes, so the host tests compile it directly.
#pragma once

#include <cstdint>
#include <cstring>
#include <string>

#include "espn_parse.h"

namespace espn {

// The team endpoint's answer for the saved team, kept in flash so a boot can
// go straight to the scoreboard instead of reading the team endpoint (about
// 23 KB) first. Only what the scoreboard request needs.
struct TeamCache {
  uint8_t league;
  uint32_t team_id;
  char event_id[16];
  int64_t kickoff_epoch;
  uint32_t group;
  int64_t saved_epoch;
} __attribute__((packed));

// Older than this, the team endpoint is read again first.
static const int64_t TEAM_CACHE_MAX_AGE = 24 * 3600;
// A cached game that kicked off longer ago than this is over: read again.
static const int64_t TEAM_CACHE_GAME_OVER = 4 * 3600;

// Fills `out` from the schedule just read for this team. False when there is
// nothing worth keeping: no next game, or an event id too long for the record.
inline bool team_cache_save(const Schedule &s, League league, uint32_t team_id, int64_t now_epoch,
                            TeamCache &out) {
  if (!s.valid || s.event_id.empty() || s.event_id.size() >= sizeof(out.event_id) || now_epoch <= 0)
    return false;
  out = TeamCache{};
  out.league = (uint8_t) league;
  out.team_id = team_id;
  memcpy(out.event_id, s.event_id.c_str(), s.event_id.size());
  out.kickoff_epoch = s.kickoff_epoch;
  out.group = s.group;
  out.saved_epoch = now_epoch;
  return true;
}

// The cached schedule, only while it can still be right: the same team,
// saved in the last day, and a game that has not been over for hours.
inline bool team_cache_load(const TeamCache &c, League league, uint32_t team_id, int64_t now_epoch,
                            Schedule &out) {
  char id[sizeof(c.event_id) + 1];
  memcpy(id, c.event_id, sizeof(c.event_id));
  id[sizeof(c.event_id)] = '\0';
  if (c.league != (uint8_t) league || c.team_id != team_id || id[0] == '\0')
    return false;
  int64_t age = now_epoch - c.saved_epoch;
  if (age < 0 || age > TEAM_CACHE_MAX_AGE)
    return false;
  if (c.kickoff_epoch < now_epoch - TEAM_CACHE_GAME_OVER)
    return false;
  out = Schedule{};
  out.valid = true;
  out.event_id = id;
  out.kickoff_epoch = c.kickoff_epoch;
  out.group = c.group;
  adopt_league(out, league);
  return true;
}

}  // namespace espn
