// Template bodies for the streaming parsers. Included from espn_parse.h.
#pragma once

#include <ArduinoJson.h>

namespace espn {
namespace detail {

inline std::string str_or_empty(JsonVariantConst v) {
  const char *s = v.as<const char *>();
  return s ? std::string(s) : std::string();
}

inline void fill_scoreboard_filter(JsonDocument &f) {
  JsonObject ev = f["events"].add<JsonObject>();
  ev["id"] = true;
  ev["date"] = true;
  ev["status"]["displayClock"] = true;
  ev["status"]["period"] = true;
  ev["status"]["type"]["state"] = true;
  ev["status"]["type"]["completed"] = true;
  ev["status"]["type"]["shortDetail"] = true;
  JsonObject comp = ev["competitions"].add<JsonObject>();
  comp["situation"]["lastPlay"]["text"] = true;
  comp["situation"]["downDistanceText"] = true;
  comp["situation"]["shortDownDistanceText"] = true;
  comp["situation"]["isRedZone"] = true;
  comp["situation"]["possession"] = true;
  comp["situation"]["homeTimeouts"] = true;
  comp["situation"]["awayTimeouts"] = true;
  JsonObject c = comp["competitors"].add<JsonObject>();
  c["id"] = true;
  c["homeAway"] = true;
  c["score"] = true;
  c["winner"] = true;
  c["records"].add<JsonObject>()["summary"] = true;
  c["team"]["abbreviation"] = true;
  c["team"]["color"] = true;
  c["team"]["logo"] = true;
  comp["odds"].add<JsonObject>()["details"] = true;
  comp["odds"][0]["overUnder"] = true;
  comp["broadcasts"].add<JsonObject>()["names"] = true;
  comp["venue"]["fullName"] = true;
}

inline void fill_scan_filter(JsonDocument &f) {
  JsonObject ev = f["events"].add<JsonObject>();
  ev["id"] = true;
  ev["status"]["type"]["state"] = true;
  JsonObject c = ev["competitions"].add<JsonObject>()["competitors"].add<JsonObject>();
  c["id"] = true;
  c["homeAway"] = true;
  c["team"]["abbreviation"] = true;
  c["team"]["conferenceId"] = true;
}

inline void fill_team_filter(JsonDocument &f) {
  f["team"]["color"] = true;
  f["team"]["record"]["items"].add<JsonObject>()["summary"] = true;
  f["team"]["groups"]["id"] = true;
  JsonObject ne = f["team"]["nextEvent"].add<JsonObject>();
  ne["id"] = true;
  ne["date"] = true;
}

inline std::string format_over_under(JsonVariantConst v) {
  if (v.isNull())
    return "";
  float f = v.as<float>();
  char buf[16];
  if (f == (int) f)
    snprintf(buf, sizeof(buf), "%d", (int) f);
  else
    snprintf(buf, sizeof(buf), "%.1f", f);
  return buf;
}

inline bool snapshot_from_event(JsonObjectConst ev, uint32_t our_team_id, GameSnapshot &out) {
  GameSnapshot s;
  s.event_id = str_or_empty(ev["id"]);
  s.kickoff_epoch = parse_iso8601_z(str_or_empty(ev["date"]));
  JsonObjectConst status = ev["status"];
  std::string state = str_or_empty(status["type"]["state"]);
  if (state == "in")
    s.state = GameState::IN;
  else if (state == "post")
    s.state = GameState::POST;
  else
    s.state = GameState::PRE;
  s.completed = status["type"]["completed"] | false;
  s.short_detail = str_or_empty(status["type"]["shortDetail"]);
  s.display_clock = str_or_empty(status["displayClock"]);
  s.period = status["period"] | 0;

  JsonObjectConst comp = ev["competitions"][0];
  if (comp.isNull())
    return false;
  JsonObjectConst sit = comp["situation"];
  s.last_play = str_or_empty(sit["lastPlay"]["text"]);
  s.down_distance = str_or_empty(sit["downDistanceText"]);
  s.short_down_distance = str_or_empty(sit["shortDownDistanceText"]);
  s.is_red_zone = sit["isRedZone"] | false;
  std::string possession = str_or_empty(sit["possession"]);
  int home_to = sit["homeTimeouts"] | 0;
  int away_to = sit["awayTimeouts"] | 0;

  bool found_us = false;
  for (JsonObjectConst c : comp["competitors"].as<JsonArrayConst>()) {
    uint32_t id = (uint32_t) atol(str_or_empty(c["id"]).c_str());
    bool home = str_or_empty(c["homeAway"]) == "home";
    std::string abbr = str_or_empty(c["team"]["abbreviation"]);
    int score = atoi(str_or_empty(c["score"]).c_str());
    std::string record = str_or_empty(c["records"][0]["summary"]);
    std::string color = str_or_empty(c["team"]["color"]);
    std::string logo = dark_logo(str_or_empty(c["team"]["logo"]));
    bool winner = c["winner"] | false;
    int timeouts = home ? home_to : away_to;
    bool ours = id == our_team_id;
    if (ours)
      found_us = true;
    std::string &t_abbr = ours ? s.team_abbr : s.opp_abbr;
    t_abbr = abbr;
    (ours ? s.team_id : s.opp_id) = id;
    (ours ? s.team_score : s.opp_score) = score;
    (ours ? s.team_record : s.opp_record) = record;
    (ours ? s.team_color : s.opp_color) = color;
    (ours ? s.team_logo : s.opp_logo) = logo;
    (ours ? s.team_timeouts : s.opp_timeouts) = timeouts;
    if (ours)
      s.team_winner = winner;
    if (!possession.empty() && possession == str_or_empty(c["id"]))
      s.possession = ours ? 1 : 2;
  }
  if (!found_us)
    return false;

  JsonObjectConst odds = comp["odds"][0];
  s.odds = str_or_empty(odds["details"]);
  s.over_under = format_over_under(odds["overUnder"]);
  s.tv = str_or_empty(comp["broadcasts"][0]["names"][0]);
  s.venue = str_or_empty(comp["venue"]["fullName"]);
  s.valid = true;
  out = s;
  return true;
}

}  // namespace detail

// Parses the team endpoint. `input` is anything ArduinoJson can read from:
// a std::string, or a reader object with read() and readBytes().
template<typename TInput> bool parse_team(TInput &input, Schedule &out) {
  JsonDocument filter;
  detail::fill_team_filter(filter);
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, input, DeserializationOption::Filter(filter),
                                              DeserializationOption::NestingLimit(40));
  if (err)
    return false;
  JsonObjectConst team = doc["team"];
  if (team.isNull())
    return false;
  Schedule s;
  s.team_color = detail::str_or_empty(team["color"]);
  s.team_record = detail::str_or_empty(team["record"]["items"][0]["summary"]);
  // ESPN sends the group id as a string ("1"), sometimes as a number.
  JsonVariantConst group = team["groups"]["id"];
  s.group = group.is<const char *>() ? (uint32_t) atol(group.as<const char *>()) : (group | 0);
  JsonObjectConst ne = team["nextEvent"][0];
  if (!ne.isNull()) {
    s.event_id = detail::str_or_empty(ne["id"]);
    s.kickoff_epoch = parse_iso8601_z(detail::str_or_empty(ne["date"]));
  }
  s.valid = true;
  out = s;
  return true;
}

// Lists the in-progress games in a scoreboard document.
template<typename TInput, typename TOut> bool parse_live_games(TInput &input, TOut &out) {
  JsonDocument filter;
  detail::fill_scan_filter(filter);
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, input, DeserializationOption::Filter(filter),
                                              DeserializationOption::NestingLimit(40));
  if (err)
    return false;
  for (JsonObjectConst ev : doc["events"].as<JsonArrayConst>()) {
    if (detail::str_or_empty(ev["status"]["type"]["state"]) != "in")
      continue;
    LiveGame g;
    g.event_id = detail::str_or_empty(ev["id"]);
    for (JsonObjectConst c : ev["competitions"][0]["competitors"].as<JsonArrayConst>()) {
      bool home = detail::str_or_empty(c["homeAway"]) == "home";
      std::string abbr = detail::str_or_empty(c["team"]["abbreviation"]);
      if (home) {
        g.home_abbr = abbr;
        g.group = (uint32_t) atol(detail::str_or_empty(c["team"]["conferenceId"]).c_str());
      } else {
        g.away_abbr = abbr;
        g.away_id = (uint32_t) atol(detail::str_or_empty(c["id"]).c_str());
      }
    }
    if (!g.event_id.empty() && g.away_id != 0)
      out.push_back(g);
  }
  return true;
}

// Parses a scoreboard document and extracts the event with `event_id`.
// Returns false on a JSON error or when the event is not in the document.
template<typename TInput>
bool parse_scoreboard(TInput &input, const std::string &event_id, uint32_t our_team_id, GameSnapshot &out) {
  JsonDocument filter;
  detail::fill_scoreboard_filter(filter);
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, input, DeserializationOption::Filter(filter),
                                              DeserializationOption::NestingLimit(40));
  if (err)
    return false;
  for (JsonObjectConst ev : doc["events"].as<JsonArrayConst>()) {
    if (detail::str_or_empty(ev["id"]) != event_id)
      continue;
    return detail::snapshot_from_event(ev, our_team_id, out);
  }
  return false;
}

}  // namespace espn
