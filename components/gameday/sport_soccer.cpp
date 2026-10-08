// Soccer's parse extras, ticker, clock row and splashes. Pure, no ESPHome
// includes.
#include "espn_parse.h"

#include <cstdio>
#include <cstdlib>

namespace espn {

using detail::add_part;
using detail::str_or_empty;

// ---- plain ASCII for the panel fonts ---------------------------------------

// U+00C0..U+017F, one ASCII letter each. '*' entries are the two-letter ones
// in fold_special().
static const char kFold[] =
    "AAAAAA*CEEEEIIIIDNOOOOOxOUUUUY**aaaaaa*ceeeeiiiidnooooo/ouuuuy*y"
    "AaAaAaCcCcCcCcDdDdEeEeEeEeEeGgGgGgGgHhHhIiIiIiIiIi**JjKkkLlLlLlLlLlNnNnNnnNnOoOoOo**RrRrRrSsSsSsSsTtTtTtUuUuUu"
    "UuUuUuWwYyYZzZzZzs";
static_assert(sizeof(kFold) == 0x180 - 0xC0 + 1, "one letter per code point");

static const char *fold_special(uint32_t cp) {
  switch (cp) {
    case 0xC6:
      return "AE";
    case 0xE6:
      return "ae";
    case 0xDE:
      return "Th";
    case 0xFE:
      return "th";
    case 0xDF:
      return "ss";
    case 0x132:
      return "IJ";
    case 0x133:
      return "ij";
    case 0x152:
      return "OE";
    case 0x153:
      return "oe";
    case 0x218:  // S and T with a comma below (Romanian)
      return "S";
    case 0x219:
      return "s";
    case 0x21A:
      return "T";
    case 0x21B:
      return "t";
    case 0x2018:
    case 0x2019:
      return "'";
    case 0x201C:
    case 0x201D:
      return "\"";
    case 0x2013:
    case 0x2014:
      return "-";
    default:
      return nullptr;
  }
}

std::string ascii_fold(const std::string &in) {
  std::string out;
  out.reserve(in.size());
  size_t i = 0;
  while (i < in.size()) {
    unsigned char c = (unsigned char) in[i];
    if (c < 0x80) {
      out += (char) c;
      i++;
      continue;
    }
    size_t len = (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3 : (c & 0xF8) == 0xF0 ? 4 : 1;
    uint32_t cp = len == 2 ? (c & 0x1F) : len == 3 ? (c & 0x0F) : (c & 0x07);
    bool ok = len > 1 && i + len <= in.size();
    for (size_t k = 1; ok && k < len; k++) {
      unsigned char cc = (unsigned char) in[i + k];
      ok = (cc & 0xC0) == 0x80;
      cp = (cp << 6) | (cc & 0x3F);
    }
    if (!ok) {
      i++;  // not UTF-8: skip the byte
      continue;
    }
    i += len;
    const char *special = fold_special(cp);
    if (special != nullptr)
      out += special;
    else if (cp >= 0xC0 && cp < 0x180)
      out += kFold[cp - 0xC0];
    // Drop the rest because the panel font has no glyph for it
  }
  return out;
}

// "B. Saka" -> "Saka". Names without an initial ("Vitinho", "Alan Patrick")
// stay whole.
static std::string player_name(const std::string &short_name) {
  size_t dot = short_name.find(". ");
  if (dot != std::string::npos && dot <= 2)
    return short_name.substr(dot + 2);
  return short_name;
}

static std::string last_word(const std::string &name) {
  size_t space = name.rfind(' ');
  return space == std::string::npos ? name : name.substr(space + 1);
}

static bool has(const std::string &s, const char *part) { return s.find(part) != std::string::npos; }

// ---- parse --------------------------------------------------------------------

static const size_t kMaxGoals = 16;

namespace detail {

void fill_soccer_filter(JsonObject ev) {
  ev["status"]["type"]["name"] = true;
  JsonObject comp = ev["competitions"][0];
  comp["altGameNote"] = true;
  JsonObject c = comp["competitors"][0];
  c["form"] = true;
  c["shootoutScore"] = true;
  JsonObject stat = c["statistics"].add<JsonObject>();
  stat["name"] = true;
  stat["displayValue"] = true;
  JsonObject d = comp["details"].add<JsonObject>();
  d["type"]["text"] = true;
  d["clock"]["displayValue"] = true;
  d["team"]["id"] = true;
  d["scoringPlay"] = true;
  d["redCard"] = true;
  d["yellowCard"] = true;
  d["penaltyKick"] = true;
  d["ownGoal"] = true;
  d["shootout"] = true;
  d["athletesInvolved"].add<JsonObject>()["shortName"] = true;
  JsonObject odds = comp["odds"][0];
  odds["drawOdds"]["moneyLine"] = true;
  for (const char *side : {"home", "away", "draw"}) {
    odds["moneyline"][side]["close"]["odds"] = true;
    odds["moneyline"][side]["open"]["odds"] = true;
  }
}

// Before kickoff the three-way line is in open and close. "OFF" means the
// book is closed.
static std::string moneyline(JsonObjectConst ml, const char *side) {
  std::string v = str_or_empty(ml[side]["close"]["odds"]);
  if (v.empty())
    v = str_or_empty(ml[side]["open"]["odds"]);
  return v == "OFF" ? "" : v;
}

void soccer_from_event(JsonObjectConst ev, GameSnapshot &s) {
  Soccer &f = s.soc;
  f.status = str_or_empty(ev["status"]["type"]["name"]);
  JsonObjectConst comp = ev["competitions"][0];
  f.note = str_or_empty(comp["altGameNote"]);
  for (JsonObjectConst c : comp["competitors"].as<JsonArrayConst>()) {
    bool ours = (uint32_t) atol(str_or_empty(c["id"]).c_str()) == s.team_id;
    // ESPN lists the newest result first; tables print it last.
    std::string form = str_or_empty(c["form"]);
    (ours ? f.team_form : f.opp_form) = std::string(form.rbegin(), form.rend());
    if (!c["shootoutScore"].isNull())
      (ours ? f.team_shootout : f.opp_shootout) = c["shootoutScore"] | 0;
    for (JsonObjectConst st : c["statistics"].as<JsonArrayConst>()) {
      std::string name = str_or_empty(st["name"]);
      std::string value = str_or_empty(st["displayValue"]);
      if (name == "possessionPct")
        (ours ? f.team_possession : f.opp_possession) = value;
      else if (name == "totalShots")
        (ours ? f.team_shots : f.opp_shots) = atoi(value.c_str());
      else if (name == "shotsOnTarget")
        (ours ? f.team_on_target : f.opp_on_target) = atoi(value.c_str());
    }
  }

  JsonObjectConst odds = comp["odds"][0];
  JsonObjectConst ml = odds["moneyline"];
  std::string home = moneyline(ml, "home"), away = moneyline(ml, "away");
  f.team_line = s.team_home ? home : away;
  f.opp_line = s.team_home ? away : home;
  f.draw_line = moneyline(ml, "draw");
  JsonVariantConst draw = odds["drawOdds"]["moneyLine"];
  if (f.draw_line.empty() && !draw.isNull()) {
    int v = draw | 0;
    f.draw_line = (v > 0 ? "+" : "") + std::to_string(v);
  }

  // Goals and cards in match order. A goal's team is the side it counts
  // for: an own goal names the other side's player.
  for (JsonObjectConst d : comp["details"].as<JsonArrayConst>()) {
    bool ours = (uint32_t) atol(str_or_empty(d["team"]["id"]).c_str()) == s.team_id;
    std::string clock = str_or_empty(d["clock"]["displayValue"]);
    std::string who = ascii_fold(str_or_empty(d["athletesInvolved"][0]["shortName"]));
    bool shootout = d["shootout"] | false;
    std::string text = (shootout ? "Shootout " : clock + " ") + str_or_empty(d["type"]["text"]);
    if (!who.empty())
      text += ": " + who;
    f.last_event = text + " (" + (ours ? s.team_abbr : s.opp_abbr) + ")";
    if (shootout)
      continue;  // shootout kicks are not in the score
    if (d["redCard"] | false)
      (ours ? f.team_reds : f.opp_reds)++;
    else if (d["yellowCard"] | false)
      (ours ? f.team_yellows : f.opp_yellows)++;
    if ((d["scoringPlay"] | false) && f.goals.size() < kMaxGoals) {
      SoccerGoal g;
      g.clock = clock;
      g.name = player_name(who);
      g.ours = ours;
      g.own_goal = d["ownGoal"] | false;
      g.penalty = d["penaltyKick"] | false;
      f.goals.push_back(g);
    }
  }
}

}  // namespace detail

// ---- clock row -----------------------------------------------------------------

// "FT", "AET", "PENS", or ESPN's own words for a game that did not finish
// ("Postponed").
static std::string final_word(const GameSnapshot &s) {
  const Soccer &f = s.soc;
  if (has(f.status, "FINAL_PEN") || (f.team_shootout >= 0 && f.opp_shootout >= 0))
    return "PENS";
  if (has(f.status, "FINAL_AET"))
    return "AET";
  if (has(f.status, "FULL_TIME") || s.completed)
    return "FT";
  return s.short_detail;
}

static bool half_time(const GameSnapshot &s) {
  return s.state == GameState::IN && (s.soc.status == "STATUS_HALFTIME" || s.short_detail == "HT");
}

// "67'", "45'+3'", "HT", "ET 105'", "PENS 3-2". A break keeps its last
// minute in displayClock, so read the status first.
std::string soccer_clock_text(const GameSnapshot &s) {
  const Soccer &f = s.soc;
  if (s.state == GameState::POST)
    return final_word(s);
  if (s.state != GameState::IN)
    return s.short_detail;
  if (has(f.status, "SHOOTOUT") || s.period >= 5) {
    std::string t = "PENS";
    if (f.team_shootout >= 0 && f.opp_shootout >= 0)
      t += " " + std::to_string(f.team_shootout) + "-" + std::to_string(f.opp_shootout);
    return t;
  }
  bool extra = s.period >= 3;
  if (half_time(s) && !extra)
    return "HT";
  if (has(f.status, "HALFTIME"))
    return extra ? "ET HT" : "HT";
  if (has(f.status, "END_OF") || has(f.status, "END_PERIOD")) {
    // A break this firmware has not seen (before extra time or penalties):
    // ESPN's words when they fit the row.
    if (!s.short_detail.empty() && s.short_detail.size() <= 10)
      return s.short_detail;
  }
  std::string clock = s.display_clock.empty() ? s.short_detail : s.display_clock;
  if (extra) {
    std::string t = "ET " + clock;
    return t.size() <= 10 ? t : clock;
  }
  return clock;
}

// ---- situation row and records -------------------------------------------------

std::string soccer_situation(const GameSnapshot &s, size_t max) {
  if (s.soc.goals.empty())
    return "";
  const SoccerGoal &g = s.soc.goals.back();
  std::string mark = g.own_goal ? " OG" : g.penalty ? " (P)" : "";
  std::string last = last_word(g.name);
  // The name first, then the mark; a long name gives way to its last word.
  const std::string *names[] = {&g.name, &last};
  for (int with_mark = 1; with_mark >= 0; with_mark--) {
    for (const std::string *name : names) {
      std::string t = g.clock;
      if (!name->empty())
        t += " " + *name;
      if (with_mark)
        t += mark;
      if (t.size() <= max)
        return t;
    }
  }
  return (g.clock + " " + last).substr(0, max);
}

std::string soccer_board_record(const std::string &wdl) {
  if (wdl.size() <= 7)
    return wdl;
  int w = 0, d = 0, l = 0;
  if (sscanf(wdl.c_str(), "%d-%d-%d", &w, &d, &l) != 3)
    return wdl;
  return std::to_string(3 * w + d) + " pts";
}

// ---- ticker --------------------------------------------------------------------

// A cup's note names its stage ("UEFA Champions League, League Phase"); a
// league game's is only the league's name, which says nothing new.
static std::string stage(const std::string &note) { return has(note, ",") ? note : ""; }

static std::string pair_line(const char *label, const GameSnapshot &s, const std::string &ours,
                             const std::string &theirs) {
  std::string out;
  if (!ours.empty())
    out = s.team_abbr + " " + ours;
  if (!theirs.empty())
    out += (out.empty() ? "" : ", ") + s.opp_abbr + " " + theirs;
  return out.empty() ? out : std::string(label) + " " + out;
}

// "ARS -275, draw +425, LEE +700"; ESPN's favorite line when the three-way
// one is missing.
static std::string odds_line(const GameSnapshot &s) {
  const Soccer &f = s.soc;
  if (f.team_line.empty() || f.opp_line.empty() || f.draw_line.empty()) {
    std::string out = s.odds;
    if (!f.draw_line.empty())
      out += (out.empty() ? "" : ", ") + std::string("draw ") + f.draw_line;
    return out;
  }
  return s.team_abbr + " " + f.team_line + ", draw " + f.draw_line + ", " + s.opp_abbr + " " + f.opp_line;
}

// "Goals ARS 58' Guimaraes, 90'+7' Saka (P); SUN 12' Isidor"
static std::string goals_line(const GameSnapshot &s) {
  std::string ours, theirs;
  for (const auto &g : s.soc.goals) {
    std::string &side = g.ours ? ours : theirs;
    if (!side.empty())
      side += ", ";
    side += g.clock + (g.name.empty() ? "" : " " + g.name) + (g.own_goal ? " (OG)" : g.penalty ? " (P)" : "");
  }
  std::string out;
  if (!ours.empty())
    out = s.team_abbr + " " + ours;
  if (!theirs.empty())
    out += (out.empty() ? "" : "; ") + s.opp_abbr + " " + theirs;
  return out.empty() ? out : "Goals " + out;
}

static std::string cards(int yellows, int reds) {
  std::string out;
  if (yellows > 0)
    out = std::to_string(yellows) + "Y";
  if (reds > 0)
    out += (out.empty() ? "" : " ") + std::to_string(reds) + "R";
  return out;
}

static std::string stats_line(const GameSnapshot &s) {
  const Soccer &f = s.soc;
  std::string out;
  bool possession = !f.team_possession.empty() && !f.opp_possession.empty() &&
                    (f.team_possession != "0" || f.opp_possession != "0");
  if (possession)
    out = "Possession " + s.team_abbr + " " + f.team_possession + "%, " + s.opp_abbr + " " + f.opp_possession + "%";
  if (f.team_shots > 0 || f.opp_shots > 0) {
    auto shots = [](int all, int on) { return std::to_string(all < 0 ? 0 : all) + " (" + std::to_string(on < 0 ? 0 : on) + ")"; };
    add_part(out, "Shots (on target) " + s.team_abbr + " " + shots(f.team_shots, f.team_on_target) + ", " +
                      s.opp_abbr + " " + shots(f.opp_shots, f.opp_on_target));
  }
  return out;
}

static std::string records_part(const GameSnapshot &s) { return pair_line("W-D-L", s, s.team_record, s.opp_record); }

// "Full time", "After extra time", "PSG win 4-3 on penalties".
static std::string final_line(const GameSnapshot &s) {
  const Soccer &f = s.soc;
  std::string w = final_word(s);
  if (w == "PENS") {
    if (f.team_shootout >= 0 && f.opp_shootout >= 0 && f.team_shootout != f.opp_shootout) {
      bool we = f.team_shootout > f.opp_shootout;
      int hi = we ? f.team_shootout : f.opp_shootout, lo = we ? f.opp_shootout : f.team_shootout;
      return (we ? s.team_abbr : s.opp_abbr) + " win " + std::to_string(hi) + "-" + std::to_string(lo) +
             " on penalties";
    }
    return "Penalties";
  }
  if (w == "AET")
    return "After extra time";
  if (w == "FT")
    return "Full time";
  return w;
}

std::string soccer_status_text(const GameSnapshot &s, const TickerOptions &o, const std::string &kickoff_local) {
  const Soccer &f = s.soc;
  std::string out;
  switch (s.state) {
    case GameState::NOT_FOUND:
      return "No upcoming game";
    case GameState::PRE:
      add_part(out, kickoff_local.empty() ? s.short_detail : kickoff_local);
      add_part(out, stage(f.note));
      add_part(out, pair_line("Form", s, f.team_form, f.opp_form));
      if (o.odds) {
        add_part(out, odds_line(s));
        if (!s.over_under.empty())
          add_part(out, "O/U " + s.over_under);
        add_part(out, s.tv);
      }
      add_part(out, s.venue);
      return ascii_fold(out);
    case GameState::POST:
      out = final_line(s);
      add_part(out, stage(f.note));
      add_part(out, goals_line(s));
      add_part(out, pair_line("Red cards", s, f.team_reds > 0 ? std::to_string(f.team_reds) : "",
                              f.opp_reds > 0 ? std::to_string(f.opp_reds) : ""));
      add_part(out, records_part(s));
      return ascii_fold(out);
    case GameState::IN:
      break;
  }
  if (half_time(s)) {
    out = "Half time";
    add_part(out, goals_line(s));
    add_part(out, pair_line("Cards", s, cards(f.team_yellows, f.team_reds), cards(f.opp_yellows, f.opp_reds)));
    add_part(out, stats_line(s));
    add_part(out, records_part(s));
    return ascii_fold(out);
  }
  if (o.down_distance) {
    add_part(out, goals_line(s));
    add_part(out, pair_line("Cards", s, cards(f.team_yellows, f.team_reds), cards(f.opp_yellows, f.opp_reds)));
    add_part(out, stats_line(s));
  }
  if (o.clock)
    add_part(out, soccer_clock_text(s));
  if (o.last_play)
    add_part(out, f.last_event);
  if (out.empty())
    out = soccer_clock_text(s);
  return ascii_fold(out);
}

// ---- cups ----------------------------------------------------------------------

// Longer than any game runs, extra time and a shootout included.
static const int64_t kGameOver = 4 * 3600;

int pick_schedule(const std::vector<Schedule> &comps, int64_t now_epoch, const std::string &done_event) {
  int best = -1, over = -1;
  for (size_t i = 0; i < comps.size(); i++) {
    const Schedule &s = comps[i];
    if (!s.valid || s.event_id.empty())
      continue;
    if (s.event_id == done_event || s.kickoff_epoch < now_epoch - kGameOver) {
      if (over < 0)
        over = (int) i;
      continue;
    }
    if (best < 0 || s.kickoff_epoch < comps[best].kickoff_epoch)
      best = (int) i;
  }
  return best >= 0 ? best : over;
}

// ---- splashes ------------------------------------------------------------------

// A goal for us is "GOAL!", theirs "LEE GOAL" (with opponent splashes on). The
// win splashes at full time; a draw gets none. A shootout decides a level
// game.
Splash soccer_splash(const GameSnapshot &prev, const GameSnapshot &cur, bool opponent_splashes, bool neutral) {
  Splash none;
  if (!prev.valid || !cur.valid || prev.event_id != cur.event_id)
    return none;
  if (prev.state == GameState::IN && cur.state == GameState::POST) {
    int us = cur.team_score, them = cur.opp_score;
    if (us == them && cur.soc.team_shootout >= 0 && cur.soc.opp_shootout >= 0) {
      us = cur.soc.team_shootout;
      them = cur.soc.opp_shootout;
    }
    if (us > them)
      return Splash{cur.team_abbr + " WINS!", parse_color(cur.team_color)};
    if (neutral && them > us)
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
