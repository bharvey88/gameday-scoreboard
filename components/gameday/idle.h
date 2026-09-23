// Idle screens: what the panel shows when its mode has nothing to show.
// Pure logic on plain values, no ESPHome includes, so the host tests cover
// the rotation and the text.
#pragma once

#include <cstdint>
#include <cstdio>
#include <ctime>
#include <string>

namespace idle {

enum Screen : uint8_t { CLOCK = 0, COUNTDOWN = 1, STANDINGS = 2, RECORD = 3, WEATHER = 4, SCREEN_COUNT = 5 };

constexpr uint8_t bit(Screen s) { return (uint8_t) (1u << s); }
constexpr uint8_t DEFAULT_SCREENS = bit(CLOCK) | bit(COUNTDOWN);
constexpr uint8_t ALL_SCREENS = (1u << SCREEN_COUNT) - 1;

inline const char *screen_name(int s) {
  static const char *const kNames[] = {"clock", "countdown", "standings", "record", "weather"};
  return s >= 0 && s < SCREEN_COUNT ? kNames[s] : "";
}

// The next screen after `current` that the owner enabled and that has data to
// show, wrapping. The clock when nothing qualifies: it always has something
// true to say, and a dark panel reads as broken.
inline int next_screen(uint8_t enabled, uint8_t available, int current) {
  uint8_t ok = enabled & available;
  for (int step = 1; step <= SCREEN_COUNT; step++) {
    int i = ((current < 0 ? -1 : current) + step) % SCREEN_COUNT;
    if (ok & (1u << i))
      return i;
  }
  return CLOCK;
}

// "3d 14h 22m" on a wide panel, "3d 14h" on one 64 px panel; "14h 22m" under
// a day; "22:15" (minutes and seconds) in the last hour, which ticks.
inline std::string countdown_text(int64_t seconds, bool wide) {
  if (seconds <= 0)
    return "Kickoff";
  char buf[24];
  int64_t d = seconds / 86400, h = (seconds % 86400) / 3600, m = (seconds % 3600) / 60, s = seconds % 60;
  if (seconds < 3600)
    snprintf(buf, sizeof(buf), "%d:%02d", (int) m, (int) s);
  else if (d == 0)
    snprintf(buf, sizeof(buf), "%dh %02dm", (int) h, (int) m);
  else if (wide)
    snprintf(buf, sizeof(buf), "%dd %dh %02dm", (int) d, (int) h, (int) m);
  else
    snprintf(buf, sizeof(buf), "%dd %dh", (int) d, (int) h);
  return buf;
}

// 12-hour clock without a leading zero: "7:42", "12:05".
inline std::string clock_text(const struct tm &t) {
  int h = t.tm_hour % 12;
  if (h == 0)
    h = 12;
  char buf[8];
  snprintf(buf, sizeof(buf), "%d:%02d", h, t.tm_min);
  return buf;
}

// "Tue Sep 23", under the clock when no game is known.
inline std::string date_text(const struct tm &t) {
  static const char *const kDays[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
  static const char *const kMonths[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                        "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
  char buf[16];
  snprintf(buf, sizeof(buf), "%s %s %d", kDays[t.tm_wday % 7], kMonths[t.tm_mon % 12], t.tm_mday);
  return buf;
}

// Standings rows, `per_page` at a time: which page is up `elapsed_s` into
// the screen, flipping every `page_s`.
inline size_t standings_page(size_t rows, size_t per_page, int64_t elapsed_s, int64_t page_s) {
  if (rows <= per_page || page_s <= 0 || elapsed_s < 0)
    return 0;
  size_t pages = (rows + per_page - 1) / per_page;
  return (size_t) (elapsed_s / page_s) % pages;
}

}  // namespace idle
