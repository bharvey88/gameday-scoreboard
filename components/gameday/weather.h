// National Weather Service (api.weather.gov): the grid lookup and the hourly
// forecast. Pure parsing, no ESPHome includes, so the host tests cover it.
#pragma once

#include <cstdio>
#include <string>

#include "espn_parse.h"  // the streaming helpers

namespace nws {

struct Weather {
  bool valid{false};
  int temp{0};             // this hour
  int high{0}, low{0};     // over the next 24 hours
  std::string condition;   // "Mostly Sunny"
  std::string unit{"F"};
};

// NWS answers a point with more than four decimals with a redirect, and the
// firmware's client does not follow redirects on this path.
inline std::string points_url(float lat, float lon) {
  char buf[96];
  snprintf(buf, sizeof(buf), "https://api.weather.gov/points/%.4f,%.4f", lat, lon);
  return buf;
}

// grid is "FWD/86,112": office and grid cell, from points().
inline std::string hourly_url(const std::string &grid) {
  return "https://api.weather.gov/gridpoints/" + grid + "/forecast/hourly";
}

inline void fill_points_filter(JsonDocument &f) {
  f["properties"]["gridId"] = true;
  f["properties"]["gridX"] = true;
  f["properties"]["gridY"] = true;
}

// The grid cell for a point, as "FWD/86,112". A few KB, parsed whole.
template<typename TInput> bool parse_points(TInput &input, std::string &grid) {
  JsonDocument filter;
  fill_points_filter(filter);
  JsonDocument doc;
  if (deserializeJson(doc, input, DeserializationOption::Filter(filter), DeserializationOption::NestingLimit(20)))
    return false;
  JsonObjectConst p = doc["properties"];
  const char *office = p["gridId"];
  if (office == nullptr || p["gridX"].isNull() || p["gridY"].isNull())
    return false;
  grid = std::string(office) + "/" + std::to_string(p["gridX"].as<int>()) + "," + std::to_string(p["gridY"].as<int>());
  return true;
}

inline void fill_period_filter(JsonDocument &f) {
  f["temperature"] = true;
  f["temperatureUnit"] = true;
  f["shortForecast"] = true;
}

// The hourly forecast is ~160 KB for a week of hours; only the first day is
// read. The first hour gives "now"; high and low span the next 24 hours,
// because "today" has two hours left at 10 PM.
template<typename TInput> bool parse_hourly(TInput &input, Weather &out, size_t hours = 24) {
  out = Weather{};
  if (!espn::detail::seek_array(input, "periods"))
    return false;
  JsonDocument filter;
  fill_period_filter(filter);
  size_t n = 0;
  bool ok = espn::detail::for_each_element(input, filter, [&](JsonObjectConst p) {
    if (p["temperature"].isNull())
      return true;
    int t = p["temperature"].as<int>();
    if (n == 0) {
      out.temp = out.high = out.low = t;
      out.condition = espn::detail::str_or_empty(p["shortForecast"]);
      std::string unit = espn::detail::str_or_empty(p["temperatureUnit"]);
      if (!unit.empty())
        out.unit = unit;
    }
    if (t > out.high)
      out.high = t;
    if (t < out.low)
      out.low = t;
    return ++n < hours;
  });
  out.valid = ok && n > 0;
  return out.valid;
}

}  // namespace nws
