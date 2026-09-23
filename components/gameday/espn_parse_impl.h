// Template bodies for the streaming parsers. Included from espn_parse.h.
#pragma once

#include <ArduinoJson.h>

#include <algorithm>
#include <cstring>

namespace espn {

// Host-side stand-in for the device's HTTP reader: same read()/readBytes()
// interface, and total() says how far the parser got.
class StringReader {
 public:
  explicit StringReader(const std::string &s) : s_(s) {}
  int read() { return this->pos_ < this->s_.size() ? (unsigned char) this->s_[this->pos_++] : -1; }
  size_t readBytes(char *dst, size_t n) {
    size_t k = std::min(n, this->s_.size() - this->pos_);
    memcpy(dst, this->s_.data() + this->pos_, k);
    this->pos_ += k;
    return k;
  }
  size_t total() const { return this->pos_; }

 private:
  const std::string &s_;
  size_t pos_{0};
};

namespace detail {

// Wraps a reader with one character of push-back, so the array walker can
// look at the next character and still hand it to ArduinoJson.
template<typename R> class PeekReader {
 public:
  explicit PeekReader(R &r) : r_(r) {}
  int read() {
    if (this->back_ >= 0) {
      int c = this->back_;
      this->back_ = -1;
      return c;
    }
    return this->r_.read();
  }
  size_t readBytes(char *dst, size_t n) {
    size_t done = 0;
    while (done < n) {
      int c = this->read();
      if (c < 0)
        break;
      dst[done++] = (char) c;
    }
    return done;
  }
  void unread(int c) { this->back_ = c; }

 private:
  R &r_;
  int back_{-1};
};

template<typename R> int next_non_ws(R &r) {
  for (;;) {
    int c = r.read();
    if (c != ' ' && c != '\n' && c != '\r' && c != '\t')
      return c;
  }
}

// One string or number value to note on the way to the array, first
// occurrence only. `pattern` ends right before the value.
struct Capture {
  const char *pattern;
  std::string *out;
  bool number;
};

// Reads up to and including the '[' that opens "key"'s array, filling the
// captures it passes. Keys and captures are matched on the raw text, which is
// safe for the ESPN and NWS documents: "events", "entries" and "periods"
// occur once, and the captured keys come before them.
template<typename R>
bool seek_array(R &r, const char *key, const Capture *caps = nullptr, size_t ncaps = 0) {
  std::string quoted = std::string("\"") + key + "\"";
  size_t keep = quoted.size();
  for (size_t i = 0; i < ncaps; i++)
    keep = std::max(keep, strlen(caps[i].pattern));
  std::string win;
  for (;;) {
    int c = r.read();
    if (c < 0)
      return false;
    win.push_back((char) c);
    if (win.size() > keep)
      win.erase(0, win.size() - keep);
    for (size_t i = 0; i < ncaps; i++) {
      size_t n = strlen(caps[i].pattern);
      if (!caps[i].out->empty() || win.size() < n || win.compare(win.size() - n, n, caps[i].pattern) != 0)
        continue;
      for (;;) {
        int v = r.read();
        if (v < 0)
          return false;
        if (caps[i].number ? !(v >= '0' && v <= '9') : v == '"') {
          win.clear();
          break;
        }
        caps[i].out->push_back((char) v);
      }
    }
    if (win.size() < quoted.size() || win.compare(win.size() - quoted.size(), quoted.size(), quoted) != 0)
      continue;
    win.clear();
    int n = next_non_ws(r);
    if (n != ':')
      continue;
    n = next_non_ws(r);
    if (n == '[')
      return true;
  }
}

// Parses the array's elements one at a time through `filter`, so only one
// element's filtered fields are ever in memory. `fn` returns false to stop
// reading; the rest of the document is then never downloaded.
template<typename R, typename F> bool for_each_element(R &r, JsonDocument &filter, F &&fn) {
  PeekReader<R> p(r);
  JsonDocument doc;
  for (;;) {
    int c = next_non_ws(p);
    if (c == ']')
      return true;
    if (c == ',')
      continue;
    if (c < 0)
      return false;
    p.unread(c);
    doc.clear();
    DeserializationError err =
        deserializeJson(doc, p, DeserializationOption::Filter(filter), DeserializationOption::NestingLimit(20));
    if (err)
      return false;
    if (!fn(doc.as<JsonObjectConst>()))
      return true;
  }
}

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

inline void fill_upcoming_filter(JsonDocument &f) {
  f["team"]["color"] = true;
  f["team"]["recordSummary"] = true;
  f["team"]["standingSummary"] = true;
  f["team"]["groups"]["id"] = true;
  JsonObject ev = f["events"].add<JsonObject>();
  ev["id"] = true;
  ev["date"] = true;
  JsonObject comp = ev["competitions"].add<JsonObject>();
  comp["neutralSite"] = true;
  comp["status"]["type"]["state"] = true;
  JsonObject c = comp["competitors"].add<JsonObject>();
  c["homeAway"] = true;
  c["team"]["id"] = true;
  c["team"]["abbreviation"] = true;
  c["team"]["shortDisplayName"] = true;
  c["score"] = true;
  comp["broadcasts"].add<JsonObject>()["media"]["shortName"] = true;
}

inline void fill_standing_entry_filter(JsonDocument &f) {
  f["team"]["id"] = true;
  f["team"]["abbreviation"] = true;
  JsonObject st = f["stats"].add<JsonObject>();
  st["type"] = true;
  st["displayValue"] = true;
}

// ESPN sends ids and scores as strings, sometimes as numbers or objects.
inline uint32_t id_of(JsonVariantConst v) {
  return v.is<const char *>() ? (uint32_t) atol(v.as<const char *>()) : (uint32_t) (v | 0);
}
inline int score_of(JsonVariantConst v) {
  if (v.is<JsonObjectConst>())
    v = v["displayValue"];
  return v.is<const char *>() ? atoi(v.as<const char *>()) : (int) (v | 0);
}

// One scoreboard event, for the live modes' scan (filter at event level).
inline void fill_scan_event_filter(JsonDocument &f) {
  f["id"] = true;
  f["date"] = true;
  f["status"]["type"]["state"] = true;
  JsonObject c = f["competitions"].add<JsonObject>()["competitors"].add<JsonObject>();
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

// The team's season schedule: games that have not started yet, in the order
// ESPN lists them (by date), up to `max`; the last finished game; and the
// team's record, color and group.
template<typename TInput>
bool parse_team_schedule(TInput &input, uint32_t our_team_id, size_t max, TeamSchedule &sched) {
  JsonDocument filter;
  detail::fill_upcoming_filter(filter);
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, input, DeserializationOption::Filter(filter),
                                              DeserializationOption::NestingLimit(40));
  if (err)
    return false;
  sched = TeamSchedule{};
  JsonObjectConst team = doc["team"];
  sched.color = detail::str_or_empty(team["color"]);
  sched.record = detail::str_or_empty(team["recordSummary"]);
  sched.standing = detail::str_or_empty(team["standingSummary"]);
  sched.group = detail::id_of(team["groups"]["id"]);
  std::vector<Upcoming> &out = sched.upcoming;
  for (JsonObjectConst ev : doc["events"].as<JsonArrayConst>()) {
    JsonObjectConst comp = ev["competitions"][0];
    std::string state = detail::str_or_empty(comp["status"]["type"]["state"]);
    if (state == "post") {
      GameResult r;
      r.valid = true;
      r.kickoff_epoch = parse_iso8601_z(detail::str_or_empty(ev["date"]));
      r.neutral = comp["neutralSite"] | false;
      for (JsonObjectConst c : comp["competitors"].as<JsonArrayConst>()) {
        bool ours = detail::id_of(c["team"]["id"]) == our_team_id;
        if (ours) {
          r.us = detail::score_of(c["score"]);
          r.home = detail::str_or_empty(c["homeAway"]) == "home";
        } else {
          r.them = detail::score_of(c["score"]);
          r.opp_abbr = detail::str_or_empty(c["team"]["abbreviation"]);
        }
      }
      sched.last = r;  // listed by date, so the last one seen is the latest
      continue;
    }
    if (state != "pre" || out.size() >= max)
      continue;
    Upcoming u;
    u.event_id = detail::str_or_empty(ev["id"]);
    u.kickoff_epoch = parse_iso8601_z(detail::str_or_empty(ev["date"]));
    u.neutral = comp["neutralSite"] | false;
    for (JsonObjectConst c : comp["competitors"].as<JsonArrayConst>()) {
      uint32_t id = (uint32_t) atol(detail::str_or_empty(c["team"]["id"]).c_str());
      bool home = detail::str_or_empty(c["homeAway"]) == "home";
      if (id == our_team_id) {
        u.home = home;
      } else {
        u.opp_id = id;
        u.opp_abbr = detail::str_or_empty(c["team"]["abbreviation"]);
        u.opp_name = detail::str_or_empty(c["team"]["shortDisplayName"]);
      }
    }
    JsonVariantConst tv = comp["broadcasts"][0]["media"]["shortName"];
    u.tv = detail::str_or_empty(tv);
    if (!u.event_id.empty() && u.opp_id != 0)
      out.push_back(u);
  }
  sched.valid = true;
  return true;
}

template<typename TInput>
bool parse_upcoming(TInput &input, uint32_t our_team_id, size_t max, std::vector<Upcoming> &out) {
  TeamSchedule sched;
  if (!parse_team_schedule(input, our_team_id, max, sched))
    return false;
  out = sched.upcoming;
  return true;
}

// One division's or conference's standings, entry by entry: a conference
// entry carries about a hundred stats, and only "total" (overall W-L) is
// kept. Stops after `max` rows.
template<typename TInput> bool parse_standings(TInput &input, size_t max, Standings &out) {
  out = Standings{};
  std::string name, short_name;
  detail::Capture caps[] = {{"\"shortName\":\"", &short_name, false}, {"\"name\":\"", &name, false}};
  if (!detail::seek_array(input, "entries", caps, 2))
    return false;
  out.title = short_name.empty() ? name : short_name;
  JsonDocument filter;
  detail::fill_standing_entry_filter(filter);
  bool ok = detail::for_each_element(input, filter, [&](JsonObjectConst e) {
    StandingRow row;
    row.team_id = detail::id_of(e["team"]["id"]);
    row.abbr = detail::str_or_empty(e["team"]["abbreviation"]);
    for (JsonObjectConst st : e["stats"].as<JsonArrayConst>()) {
      if (detail::str_or_empty(st["type"]) == "total") {
        row.record = detail::str_or_empty(st["displayValue"]);
        break;
      }
    }
    if (!row.abbr.empty())
      out.rows.push_back(row);
    return out.rows.size() < max;
  });
  out.valid = ok && !out.rows.empty();
  return out.valid;
}

// Walks a scoreboard document event by event and sorts the games into
// in progress, final, and the earliest one not started. Works for a day's
// scoreboard (dates=) and for a week view (no date).
//
// With now_epoch set, reading stops at the first unstarted game that kicks
// off more than SCAN_STOP_MARGIN from now. ESPN lists a game day in
// progress first, then finals, then games to come, each group by kickoff;
// on a quiet day the list is simply by kickoff. Either way nothing after that
// game has started, so the rest (most of a 1.3 MB Saturday) is never read.
// The margin keeps a game that is a few minutes late starting from ending
// the read before a game with the same kickoff that did start.
template<typename TInput> bool parse_scan(TInput &input, ScanResult &out, int64_t now_epoch = 0) {
  out = ScanResult{};
  std::string week;
  detail::Capture caps[] = {{"\"week\":{\"number\":", &week, true}};
  if (!detail::seek_array(input, "events", caps, 1))
    return false;
  JsonDocument filter;
  detail::fill_scan_event_filter(filter);
  bool ok = detail::for_each_element(input, filter, [&](JsonObjectConst ev) {
    out.events++;
    std::string state = detail::str_or_empty(ev["status"]["type"]["state"]);
    LiveGame g;
    g.event_id = detail::str_or_empty(ev["id"]);
    g.kickoff_epoch = parse_iso8601_z(detail::str_or_empty(ev["date"]));
    g.state = state == "in" ? GameState::IN : state == "post" ? GameState::POST : GameState::PRE;
    for (JsonObjectConst c : ev["competitions"][0]["competitors"].as<JsonArrayConst>()) {
      bool home = detail::str_or_empty(c["homeAway"]) == "home";
      std::string abbr = detail::str_or_empty(c["team"]["abbreviation"]);
      uint32_t id = (uint32_t) atol(detail::str_or_empty(c["id"]).c_str());
      if (home) {
        g.home_abbr = abbr;
        g.home_id = id;
        g.group = (uint32_t) atol(detail::str_or_empty(c["team"]["conferenceId"]).c_str());
      } else {
        g.away_abbr = abbr;
        g.away_id = id;
      }
    }
    if (g.event_id.empty() || g.away_id == 0)
      return true;
    if (g.state == GameState::IN)
      out.live.push_back(g);
    else if (g.state == GameState::POST)
      out.finals.push_back(g);
    else if (out.later.state != GameState::PRE || g.kickoff_epoch < out.later.kickoff_epoch)
      out.later = g;
    if (now_epoch != 0 && g.state == GameState::PRE && g.kickoff_epoch > now_epoch + SCAN_STOP_MARGIN) {
      out.stopped = true;
      return false;
    }
    return true;
  });
  out.week = atoi(week.c_str());
  return ok;
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
