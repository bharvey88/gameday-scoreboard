// Timezone choices offered on the device. scripts and select.py parse this
// table, so keep each entry on one line in this exact shape.
#pragma once

namespace gameday {

struct Timezone {
  const char *name;
  const char *posix;
};

constexpr Timezone kTimezones[] = {
    {"US Eastern", "EST5EDT,M3.2.0,M11.1.0"},
    {"US Central", "CST6CDT,M3.2.0,M11.1.0"},
    {"US Mountain", "MST7MDT,M3.2.0,M11.1.0"},
    {"US Arizona", "MST7"},
    {"US Pacific", "PST8PDT,M3.2.0,M11.1.0"},
    {"US Alaska", "AKST9AKDT,M3.2.0,M11.1.0"},
    {"US Hawaii", "HST10"},
    {"UTC", "UTC0"},
    {"UK", "GMT0BST,M3.5.0/1,M10.5.0"},
    {"Central Europe", "CET-1CEST,M3.5.0,M10.5.0/3"},
};

constexpr size_t kTimezoneCount = sizeof(kTimezones) / sizeof(kTimezones[0]);
constexpr size_t kDefaultTimezone = 1;  // US Central

}  // namespace gameday
