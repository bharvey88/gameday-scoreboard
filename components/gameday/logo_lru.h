#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace espn {

struct LogoSlot {
  uint32_t used;  // millis() of the last lookup
  bool busy;      // queued or downloading
  bool pinned;    // on screen right now
};

// Index of the least recently used slot that can be dropped, or -1 when every
// slot is busy or on screen.
inline int pick_evict(const std::vector<LogoSlot> &slots) {
  int best = -1;
  for (size_t i = 0; i < slots.size(); i++) {
    if (slots[i].busy || slots[i].pinned)
      continue;
    if (best < 0 || slots[i].used < slots[(size_t) best].used)
      best = (int) i;
  }
  return best;
}

// A logo that failed is asked for again once this long has passed.
constexpr uint32_t LOGO_RETRY_MS = 15000;

inline bool logo_retry_due(uint32_t failed_ms, uint32_t now) { return now - failed_ms >= LOGO_RETRY_MS; }

}  // namespace espn
