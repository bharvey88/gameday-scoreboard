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
};

struct FavRules {
  int64_t lockon_s{15 * 60};  // a game takes the panel this long before kickoff
  int64_t release_s{30};      // and keeps it this long after the final
  bool alternate{false};      // two locked games: flip between them (else the higher slot wins)
  int64_t rotate_s{5 * 60};   // flip interval when alternating
  int64_t dwell_s{10};        // seconds per card in the idle playlist
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
  if (valid(current) && now - shown_since < r.dwell_s)
    c.index = current;
  else
    c.index = fav_next_(games, current, valid);
  return c;
}

}  // namespace espn
