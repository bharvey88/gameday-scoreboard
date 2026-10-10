// Basketball's parse extras, ticker, clock row and splash (NBA, WNBA, men's
// college). Pure, no ESPHome includes.
#include "espn_parse.h"

namespace espn {

using detail::add_part;
using detail::records_line;

namespace detail {

void fill_basketball_filter(JsonObject ev) {
  ev["status"]["type"]["name"] = true;
  JsonObject comp = ev["competitions"][0];
  comp["format"]["regulation"]["periods"] = true;
  JsonObject series = comp["series"].to<JsonObject>();
  series["type"] = true;
  series["summary"] = true;
  JsonObject c = series["competitors"].add<JsonObject>();
  c["id"] = true;
  c["wins"] = true;
}

void basketball_from_event(JsonObjectConst ev, GameSnapshot &s) {
  Basketball &b = s.nba;
  JsonObjectConst comp = ev["competitions"][0];
  int periods = comp["format"]["regulation"]["periods"] | 4;
  b.regulation = (uint8_t) (periods > 0 && periods < 10 ? periods : 4);
  std::string name = str_or_empty(ev["status"]["type"]["name"]);
  if (name == "STATUS_IN_PROGRESS")
    b.phase = Phase::PLAY;
  else if (name == "STATUS_END_PERIOD")
    b.phase = Phase::END_PERIOD;
  else if (name == "STATUS_HALFTIME")
    b.phase = Phase::HALFTIME;
  // Keep the series of a playoff game only
  JsonObjectConst series = comp["series"];
  if (str_or_empty(series["type"]) != "playoff")
    return;
  b.series = str_or_empty(series["summary"]);
  for (JsonObjectConst c : series["competitors"].as<JsonArrayConst>()) {
    uint32_t id = (uint32_t) atol(str_or_empty(c["id"]).c_str());
    if (c["wins"].isNull())
      continue;
    int8_t wins = (int8_t) (c["wins"] | 0);
    if (id == s.team_id)
      b.team_wins = wins;
    else if (id == s.opp_id)
      b.opp_wins = wins;
  }
}

}  // namespace detail

// "1st".."4th" in regulation (halves in college), then "OT", "2OT", ...
static std::string period_label(const GameSnapshot &s) {
  static const char *kOrdinals[] = {"1st", "2nd", "3rd", "4th"};
  int reg = s.nba.regulation;
  if (s.period <= 0)
    return "";
  if (s.period <= reg)
    return s.period <= 4 ? kOrdinals[s.period - 1] : std::to_string(s.period) + "th";
  int ot = s.period - reg;
  return ot == 1 ? "OT" : std::to_string(ot) + "OT";
}

std::string basketball_series(const GameSnapshot &s) {
  const Basketball &b = s.nba;
  if (b.team_wins < 0 || b.opp_wins < 0)
    return "";
  int hi = b.team_wins > b.opp_wins ? b.team_wins : b.opp_wins;
  int lo = b.team_wins > b.opp_wins ? b.opp_wins : b.team_wins;
  std::string score = std::to_string(hi) + "-" + std::to_string(lo);
  if (hi == lo)
    return "Series " + score;
  const std::string &leader = b.team_wins > b.opp_wins ? s.team_abbr : s.opp_abbr;
  std::string out = leader + " lead " + score;
  // Say "up" when "lead" overflows the 12-character row ("UTAH lead 2-1")
  return out.size() <= 12 ? out : leader + " up " + score;
}

std::string basketball_status_text(const GameSnapshot &s, const TickerOptions &o, const std::string &kickoff_local) {
  const Basketball &b = s.nba;
  std::string out;
  switch (s.state) {
    case GameState::NOT_FOUND:
      return "No upcoming game";
    case GameState::PRE:
      add_part(out, kickoff_local.empty() ? s.short_detail : kickoff_local);
      add_part(out, b.series);
      if (o.odds) {
        add_part(out, s.odds);
        if (!s.over_under.empty())
          add_part(out, "O/U " + s.over_under);
        add_part(out, s.tv);
      }
      add_part(out, s.venue);
      return out;
    case GameState::POST:
      // Postponed, Canceled or Suspended: ESPN's post state without a result.
      if (!s.completed && !s.short_detail.empty())
        return s.short_detail;
      // "Final/OT", "Final/2OT" after overtime
      out = s.short_detail.rfind("Final", 0) == 0 ? s.short_detail : "Final";
      add_part(out, b.series.empty() ? records_line(s) : b.series);
      return out;
    case GameState::IN:
      break;
  }
  if (b.phase == Phase::HALFTIME || b.phase == Phase::END_PERIOD) {
    // The clock row says which break it is.
    out = b.series.empty() ? records_line(s) : b.series;
    return out.empty() ? s.short_detail : out;
  }
  if (o.clock)
    add_part(out, s.short_detail);
  if (o.last_play) {
    std::string play = s.last_play;
    if (play.size() > 160)
      play = play.substr(0, 157) + "...";
    add_part(out, play);
  }
  if (out.empty())
    out = s.short_detail.empty() ? "In Progress" : s.short_detail;
  return out;
}

// "1:04 4th", "45.3 4th", "2:10 OT", "Halftime", "End 3rd".
// ESPN drops the minutes under a minute.
// The period is named from its number so college halves read "12:34 2nd",
// which fits the clock row's 10 characters.
std::string basketball_clock_text(const GameSnapshot &s) {
  const Basketball &b = s.nba;
  if (s.state == GameState::IN && s.period > 0) {
    if (b.phase == Phase::HALFTIME)
      return "Halftime";
    if (b.phase == Phase::END_PERIOD)
      return "End " + period_label(s);
    if (b.phase == Phase::PLAY && !s.display_clock.empty())
      return s.display_clock + " " + period_label(s);
  }
  std::string t = s.short_detail;  // "Delayed" and the like
  size_t dash = t.find(" - ");
  if (dash != std::string::npos)
    t.replace(dash, 3, " ");
  return t;
}

// Win only: a basketball game has around a hundred scoring plays.
Splash basketball_splash(const GameSnapshot &prev, const GameSnapshot &cur, bool /*opponent_splashes*/,
                         bool neutral) {
  Splash none;
  if (!prev.valid || !cur.valid || prev.event_id != cur.event_id)
    return none;
  if (prev.state != GameState::IN || cur.state != GameState::POST || !cur.completed)
    return none;
  if (cur.team_score > cur.opp_score)
    return Splash{cur.team_abbr + " WINS!", parse_color(cur.team_color)};
  if (neutral && cur.opp_score > cur.team_score)
    return Splash{cur.opp_abbr + " WINS!", parse_color(cur.opp_color)};
  return none;
}

}  // namespace espn
