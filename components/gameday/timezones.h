// Timezone choices offered on the device. select.py and the web page parse
// this table, so keep each entry on one line in this exact shape:
//     {"Display name", "POSIX TZ string", "IANA/Zone"},
// The IANA name is what browsers report; the page uses it to suggest a zone.
#pragma once

#include <cstddef>
#include <cstdlib>
#include <cstring>

namespace espn {

struct Timezone {
  const char *name;
  const char *posix;
  const char *iana;
};

constexpr Timezone kTimezones[] = {
    {"US Eastern", "EST5EDT,M3.2.0,M11.1.0", "America/New_York"},
    {"US Central", "CST6CDT,M3.2.0,M11.1.0", "America/Chicago"},
    {"US Mountain", "MST7MDT,M3.2.0,M11.1.0", "America/Denver"},
    {"US Arizona", "MST7", "America/Phoenix"},
    {"US Pacific", "PST8PDT,M3.2.0,M11.1.0", "America/Los_Angeles"},
    {"US Alaska", "AKST9AKDT,M3.2.0,M11.1.0", "America/Anchorage"},
    {"US Hawaii", "HST10", "Pacific/Honolulu"},
    {"Canada Atlantic", "AST4ADT,M3.2.0,M11.1.0", "America/Halifax"},
    {"Canada Newfoundland", "NST3:30NDT,M3.2.0,M11.1.0", "America/St_Johns"},
    {"Mexico City", "CST6", "America/Mexico_City"},
    {"UK and Ireland", "GMT0BST,M3.5.0/1,M10.5.0", "Europe/London"},
    {"Central Europe", "CET-1CEST,M3.5.0,M10.5.0/3", "Europe/Berlin"},
    {"Australia Eastern", "AEST-10AEDT,M10.1.0,M4.1.0/3", "Australia/Sydney"},
    {"UTC", "UTC0", "UTC"},
};

constexpr size_t kTimezoneCount = sizeof(kTimezones) / sizeof(kTimezones[0]);
constexpr size_t kDefaultTimezone = 1;  // US Central

// Index of the zone a tz= value names: a display name ("US Central"), an IANA
// name ("America/Chicago") or a position in this list ("1"). -1 if nothing
// matches. Positions move when the list changes, so the app sends a name.
inline int timezone_index(const char *value) {
  if (value == nullptr || *value == '\0')
    return -1;
  bool digits = true;
  for (const char *c = value; *c != '\0'; c++) {
    if (*c < '0' || *c > '9') {
      digits = false;
      break;
    }
  }
  if (digits) {
    unsigned long i = strtoul(value, nullptr, 10);
    return i < kTimezoneCount ? (int) i : -1;
  }
  for (size_t i = 0; i < kTimezoneCount; i++) {
    if (strcmp(value, kTimezones[i].name) == 0 || strcmp(value, kTimezones[i].iana) == 0)
      return (int) i;
  }
  return -1;
}

}  // namespace espn
