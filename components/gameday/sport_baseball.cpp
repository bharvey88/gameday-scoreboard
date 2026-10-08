// Baseball's ticker, clock row and splashes. Pure, no ESPHome includes.
#include "espn_parse.h"

namespace espn {

using detail::add_part;
using detail::records_line;

static std::string hits_line(const GameSnapshot &s) {
  const Baseball &b = s.mlb;
  return s.team_abbr + " " + std::to_string(b.team_hits) + " H " + std::to_string(b.team_errors) + " E, " +
         s.opp_abbr + " " + std::to_string(b.opp_hits) + " H " + std::to_string(b.opp_errors) + " E";
}

std::string baseball_count(const GameSnapshot &s) {
  const Baseball &b = s.mlb;
  if (s.state != GameState::IN || b.balls < 0 || b.strikes < 0)
    return "";
  return std::to_string(b.balls) + "-" + std::to_string(b.strikes);
}

std::string baseball_status_text(const GameSnapshot &s, const TickerOptions &o, const std::string &kickoff_local) {
  const Baseball &b = s.mlb;
  std::string out;
  switch (s.state) {
    case GameState::NOT_FOUND:
      return "No upcoming game";
    case GameState::PRE:
      add_part(out, kickoff_local.empty() ? s.short_detail : kickoff_local);
      add_part(out, b.series);
      if (!b.team_probable.empty() && !b.opp_probable.empty())
        add_part(out, s.team_abbr + " " + b.team_probable + " vs " + s.opp_abbr + " " + b.opp_probable);
      if (o.odds) {
        add_part(out, s.odds);
        if (!s.over_under.empty())
          add_part(out, "O/U " + s.over_under);
        add_part(out, s.tv);
      }
      add_part(out, s.venue);
      return out;
    case GameState::POST:
      // "Final/10" after extra innings
      out = s.short_detail.rfind("Final", 0) == 0 ? s.short_detail : "Final";
      add_part(out, hits_line(s));
      add_part(out, b.series.empty() ? records_line(s) : b.series);
      return out;
    case GameState::IN:
      break;
  }
  if (b.half != Half::TOP && b.half != Half::BOTTOM) {
    // Between half innings, or a delay: the clock row says which.
    out = hits_line(s);
    add_part(out, b.series);
    return out;
  }
  if (o.down_distance) {
    std::string count = baseball_count(s);
    if (b.outs >= 0)
      count += (count.empty() ? "" : ", ") + std::to_string(b.outs) + (b.outs == 1 ? " out" : " outs");
    add_part(out, count);
    if (!b.pitcher.empty() && !b.batter.empty())
      add_part(out, "P " + b.pitcher + ", AB " + b.batter);
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

// "Top 5th", "Mid 5th", "Rain Delay": ESPN's own words, there is no clock.
std::string baseball_clock_text(const GameSnapshot &s) { return s.short_detail; }

static bool home_run(const GameSnapshot &s) {
  return s.mlb.play_type == "Home Run" || s.last_play.find(" homered") != std::string::npos;
}

static std::string runs_word(int runs, bool hr, bool ours, const std::string &abbr) {
  if (ours) {
    if (hr)
      return "HOME RUN!";
    return runs == 1 ? "RUN!" : std::to_string(runs) + " RUNS!";
  }
  if (hr)
    return abbr + " HOME RUN";
  return runs == 1 ? abbr + " SCORES" : abbr + " " + std::to_string(runs) + " RUNS";
}

Splash baseball_splash(const GameSnapshot &prev, const GameSnapshot &cur, bool opponent_splashes, bool neutral) {
  Splash none;
  if (!prev.valid || !cur.valid || prev.event_id != cur.event_id)
    return none;
  if (prev.state == GameState::IN && cur.state == GameState::POST) {
    if (cur.team_score > cur.opp_score)
      return Splash{cur.team_abbr + " WINS!", parse_color(cur.team_color)};
    if (neutral && cur.opp_score > cur.team_score)
      return Splash{cur.opp_abbr + " WINS!", parse_color(cur.opp_color)};
    return none;
  }
  if (prev.state != GameState::IN || cur.state != GameState::IN)
    return none;
  bool hr = home_run(cur);
  int delta = cur.team_score - prev.team_score;
  if (delta > 0)
    return Splash{runs_word(delta, hr, !neutral, cur.team_abbr), parse_color(cur.team_color)};
  int odelta = cur.opp_score - prev.opp_score;
  if (odelta > 0 && opponent_splashes)
    return Splash{runs_word(odelta, hr, false, cur.opp_abbr), parse_color(cur.opp_color)};
  return none;
}

}  // namespace espn
