// Hockey's ticker, clock row and splashes. Pure, no ESPHome includes.
// ESPN's NHL feed has no power play field. The board must not infer one from
// penalties, because offsetting and coincidental minors would make it wrong.
#include "espn_parse.h"

namespace espn {

using detail::add_part;
using detail::records_line;

namespace detail {

void fill_hockey_filter(JsonObject ev) {
  ev["season"]["type"] = true;
  ev["competitions"][0]["series"]["summary"] = true;
}

void hockey_from_event(JsonObjectConst ev, GameSnapshot &s) {
  s.nhl.playoffs = (ev["season"]["type"] | 0) == 3;
  s.nhl.series = str_or_empty(ev["competitions"][0]["series"]["summary"]);
}

}  // namespace detail

static bool shootout(const GameSnapshot &s) { return s.period >= 5 && !s.nhl.playoffs; }

// Between periods ESPN's shortDetail reads "End of 1st" (status END_PERIOD)
static bool intermission(const GameSnapshot &s) {
  return s.state == GameState::IN && s.short_detail.rfind("End of", 0) == 0;
}

static std::string period_name(const GameSnapshot &s) {
  switch (s.period) {
    case 1:
      return "1st";
    case 2:
      return "2nd";
    case 3:
      return "3rd";
    case 4:
      return "OT";
    default:
      if (s.period < 1)
        return "";
      return shootout(s) ? "SO" : std::to_string(s.period - 3) + "OT";  // playoffs: 2OT, 3OT
  }
}

// "12:34 2nd", "3:10 OT", "SO", or ESPN's own words between periods ("End of 1st")
std::string hockey_clock_text(const GameSnapshot &s) {
  if (s.state == GameState::IN && shootout(s))
    return "SO";
  bool has_clock = s.display_clock.find(':') != std::string::npos && s.period > 0;
  bool special = s.short_detail.find(':') == std::string::npos;
  if (!has_clock || special) {
    std::string t = s.short_detail;
    size_t dash = t.find(" - ");
    if (dash != std::string::npos)
      t.replace(dash, 3, " ");
    return t;
  }
  return s.display_clock + " " + period_name(s);
}

std::string hockey_status_text(const GameSnapshot &s, const TickerOptions &o, const std::string &kickoff_local) {
  const Hockey &h = s.nhl;
  // The series line replaces the W-L-OTL records in the playoffs
  const std::string standing = h.series.empty() ? records_line(s) : h.series;
  std::string out;
  switch (s.state) {
    case GameState::NOT_FOUND:
      return "No upcoming game";
    case GameState::PRE:
      add_part(out, kickoff_local.empty() ? s.short_detail : kickoff_local);
      add_part(out, h.series);
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
      // "Final/OT", "Final/SO", "Final/2OT"
      out = s.short_detail.rfind("Final", 0) == 0 ? s.short_detail : "Final";
      add_part(out, standing);
      return out;
    case GameState::IN:
      break;
  }
  if (intermission(s)) {
    // The clock row already says which period ended
    out = standing;
    return out.empty() ? s.short_detail : out;
  }
  if (o.clock)
    add_part(out, s.short_detail);
  if (o.last_play) {
    std::string play = s.last_play;
    // A fresh goal can arrive before its scorer: ", assists: N. Pionk (1)"
    if (play.rfind(", ", 0) == 0)
      play.erase(0, 2);
    if (play.size() > 160)
      play = play.substr(0, 157) + "...";
    add_part(out, play);
  }
  if (out.empty())
    out = s.short_detail.empty() ? "In Progress" : s.short_detail;
  return out;
}

// "GOAL!" for us, "PIT GOAL" for them or for either side in the live modes.
// A shootout winner's extra goal splashes like any other goal.
Splash hockey_splash(const GameSnapshot &prev, const GameSnapshot &cur, bool opponent_splashes, bool neutral) {
  Splash none;
  if (!prev.valid || !cur.valid || prev.event_id != cur.event_id)
    return none;
  if (prev.state == GameState::IN && cur.state == GameState::POST) {
    if (!cur.completed)
      return none;
    if (cur.team_score > cur.opp_score)
      return Splash{cur.team_abbr + " WINS!", parse_color(cur.team_color)};
    if (neutral && cur.opp_score > cur.team_score)
      return Splash{cur.opp_abbr + " WINS!", parse_color(cur.opp_color)};
    return none;
  }
  if (prev.state != GameState::IN || cur.state != GameState::IN)
    return none;
  if (cur.team_score > prev.team_score)
    return Splash{neutral ? cur.team_abbr + " GOAL" : "GOAL!", parse_color(cur.team_color)};
  if (cur.opp_score > prev.opp_score && opponent_splashes)
    return Splash{cur.opp_abbr + " GOAL", parse_color(cur.opp_color)};
  return none;
}

}  // namespace espn
