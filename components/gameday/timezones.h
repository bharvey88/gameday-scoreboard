// Timezone choices offered on the device. select.py and the web page parse
// this table, so keep each entry on one line in this exact shape:
//     {"Display name", "POSIX TZ string", "IANA/Zone"},
// The IANA name is what browsers report; the page uses it to suggest a zone.
#pragma once

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
    {"Canada Saskatchewan", "CST6", "America/Regina"},
    {"Mexico City", "CST6", "America/Mexico_City"},
    {"Puerto Rico", "AST4", "America/Puerto_Rico"},
    {"Brazil (Sao Paulo)", "<-03>3", "America/Sao_Paulo"},
    {"Argentina", "<-03>3", "America/Argentina/Buenos_Aires"},
    {"UTC", "UTC0", "UTC"},
    {"UK and Ireland", "GMT0BST,M3.5.0/1,M10.5.0", "Europe/London"},
    {"Portugal", "WET0WEST,M3.5.0/1,M10.5.0", "Europe/Lisbon"},
    {"Central Europe", "CET-1CEST,M3.5.0,M10.5.0/3", "Europe/Berlin"},
    {"Eastern Europe", "EET-2EEST,M3.5.0/3,M10.5.0/4", "Europe/Athens"},
    {"Moscow", "MSK-3", "Europe/Moscow"},
    {"Israel", "IST-2IDT,M3.4.4/26,M10.5.0", "Asia/Jerusalem"},
    {"Dubai", "<+04>-4", "Asia/Dubai"},
    {"India", "IST-5:30", "Asia/Kolkata"},
    {"Singapore", "<+08>-8", "Asia/Singapore"},
    {"China", "CST-8", "Asia/Shanghai"},
    {"Japan", "JST-9", "Asia/Tokyo"},
    {"Korea", "KST-9", "Asia/Seoul"},
    {"Australia Eastern", "AEST-10AEDT,M10.1.0,M4.1.0/3", "Australia/Sydney"},
    {"Australia Brisbane", "AEST-10", "Australia/Brisbane"},
    {"Australia Central", "ACST-9:30ACDT,M10.1.0,M4.1.0/3", "Australia/Adelaide"},
    {"Australia Western", "AWST-8", "Australia/Perth"},
    {"New Zealand", "NZST-12NZDT,M9.5.0,M4.1.0/3", "Pacific/Auckland"},
};

constexpr size_t kTimezoneCount = sizeof(kTimezones) / sizeof(kTimezones[0]);
constexpr size_t kDefaultTimezone = 1;  // US Central

}  // namespace espn
