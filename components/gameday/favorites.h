// Favorite Teams mode: which of the four favorites the panel shows right now.
// Pure logic on epoch seconds, no ESPHome includes, so the host tests cover
// the timing rules (lock-on, release, collisions, the idle playlist).
#pragma once

#include <cstdint>
#include <vector>

#include "espn_parse.h"

namespace espn {

// What the decision needs to know about one favorite's next game.
struct FavGame {
  bool valid{false};  // a card has been fetched for this slot
  GameState state{GameState::NOT_FOUND};
  int64_t kickoff_epoch{0};
  int64_t final_epoch{0};  // when the panel first saw it go final; 0 = fetched already final
  // How far ahead (or back, for a final) a card may be and still cycle while
  // idle; 0 = no limit. Football has none, so a bye week never hides a team;
  // other leagues use it to drop a team that is out of season.
  int64_t horizon_s{0};
};

struct FavRules {
  int64_t lockon_s{15 * 60};  // a game takes the panel this long before kickoff
  int64_t release_s{30};      // and keeps it this long after the final
  bool alternate{false};      // two locked games: flip between them (else the higher slot wins)
  int64_t rotate_s{5 * 60};   // flip interval when alternating
  int64_t dwell_s{10};        // seconds per card in the idle playlist
  bool today_only{false};     // idle playlist: only games today (local time) or live
  int64_t day_start{0};       // today's local midnight, epoch seconds
  int64_t day_end{0};         // the next one
};

struct FavChoice {
  int index{-1};  // slot to show, -1 when no favorite has a card
  bool locked{false};
};

inline bool fav_locked(const FavGame &g, int64_t now, const FavRules &r) {
  if (!g.valid)
    return false;
  switch (g.state) {
    case GameState::IN:
      return true;
    case GameState::PRE:
      // Includes kickoffs already past: ESPN can report PRE a poll or two late.
      return g.kickoff_epoch - now <= r.lockon_s;
    case GameState::POST:
      return g.final_epoch != 0 && now - g.final_epoch < r.release_s;
    default:
      return false;
  }
}

// May the idle playlist show this card? `today` applies the today-only filter.
inline bool fav_idle_ok(const FavGame &g, int64_t now, const FavRules &r, bool today) {
  if (!g.valid)
    return false;
  if (g.state == GameState::IN)
    return true;
  if (g.horizon_s > 0) {
    if (g.state == GameState::PRE && g.kickoff_epoch - now > g.horizon_s)
      return false;
    if (g.state == GameState::POST && now - g.kickoff_epoch > g.horizon_s)
      return false;
  }
  if (!today)
    return true;
  return g.kickoff_epoch >= r.day_start && g.kickoff_epoch < r.day_end;
}

// Next entry after `from` that satisfies `ok`, wrapping; -1 if none.
template<typename F> static int fav_next_(const std::vector<FavGame> &games, int from, F ok) {
  int n = (int) games.size();
  for (int step = 1; step <= n; step++) {
    int i = ((from < 0 ? -1 : from) + step) % n;
    if (i < 0)
      i += n;
    if (ok(i))
      return i;
  }
  return -1;
}

// games: slot order = priority. current/shown_since: what is up now and since
// when. pinned: a slot picked with the remote, -1 for none.
inline FavChoice pick_favorite(const std::vector<FavGame> &games, int64_t now, const FavRules &r, int current,
                               int64_t shown_since, int pinned) {
  FavChoice c;
  if (games.empty())
    return c;
  auto locked = [&](int i) { return i >= 0 && i < (int) games.size() && fav_locked(games[i], now, r); };
  auto valid = [&](int i) { return i >= 0 && i < (int) games.size() && games[i].valid; };
  bool any_locked = false;
  for (int i = 0; i < (int) games.size(); i++)
    any_locked = any_locked || locked(i);

  if (any_locked) {
    c.locked = true;
    if (locked(pinned)) {
      c.index = pinned;
    } else if (!r.alternate) {
      for (int i = 0; i < (int) games.size(); i++)
        if (locked(i)) {
          c.index = i;
          break;
        }
    } else if (locked(current) && now - shown_since < r.rotate_s) {
      c.index = current;
    } else {
      c.index = fav_next_(games, current, locked);
    }
    return c;
  }

  // Idle playlist. A pinned card seeds it; dwell then moves on like any other.
  // Cards that pass the filters cycle. With none passing, the filters relax
  // one at a time (today only, then the season horizon) so the panel never
  // goes blank while it has cards.
  int level = 0;
  auto ok_at = [&](int i, int lv) {
    if (!valid(i))
      return false;
    if (lv >= 2)
      return true;
    return fav_idle_ok(games[i], now, r, lv == 0 && r.today_only);
  };
  for (; level < 2; level++) {
    bool any = false;
    for (int i = 0; i < (int) games.size() && !any; i++)
      any = ok_at(i, level);
    if (any)
      break;
  }
  auto ok = [&](int i) { return ok_at(i, level); };
  if (ok(current) && now - shown_since < r.dwell_s)
    c.index = current;
  else
    c.index = fav_next_(games, current, ok);
  return c;
}

}  // namespace espn
