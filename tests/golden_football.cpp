// Football output pinned to fixtures/football_golden.txt. Adding sports must
// not change a byte of what football parses, prints or requests, so this
// renders everything the pure layer produces for the football fixtures and
// compares it with the file recorded before the multi-sport work began.
// GOLDEN_WRITE=1 rewrites the file instead of comparing.
#include <ArduinoJson.h>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "espn_parse.h"

using namespace espn;

namespace {

std::string slurp(const char *path) {
  std::ifstream f(path, std::ios::binary);
  std::stringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

void dump_snapshot(std::ostream &o, const GameSnapshot &s) {
  o << "  valid=" << s.valid << " state=" << state_name(s.state) << " event=" << s.event_id
    << " kick=" << s.kickoff_epoch << " completed=" << s.completed << "\n";
  o << "  detail=" << s.short_detail << " clock=" << s.display_clock << " period=" << s.period << "\n";
  o << "  us=" << s.team_abbr << "#" << s.team_id << " " << s.team_score << " rec=" << s.team_record
    << " color=" << s.team_color << " logo=" << s.team_logo << " winner=" << s.team_winner
    << " to=" << s.team_timeouts << "\n";
  o << "  them=" << s.opp_abbr << "#" << s.opp_id << " " << s.opp_score << " rec=" << s.opp_record
    << " color=" << s.opp_color << " logo=" << s.opp_logo << " to=" << s.opp_timeouts << "\n";
  o << "  poss=" << s.possession << " dd=" << s.down_distance << " sdd=" << s.short_down_distance
    << " rz=" << s.is_red_zone << "\n";
  o << "  play=" << s.last_play << "\n";
  o << "  odds=" << s.odds << " ou=" << s.over_under << " tv=" << s.tv << " venue=" << s.venue << "\n";
}

// Every ticker flag combination, with and without a kickoff label. Equal
// outputs are listed once with the combinations that produce them.
void dump_text(std::ostream &o, const GameSnapshot &s) {
  o << "  clock_text=" << clock_text(s) << "\n";
  std::vector<std::pair<std::string, std::string>> seen;  // text, combos
  for (int m = 0; m < 32; m++) {
    TickerOptions t;
    t.clock = m & 1;
    t.down_distance = m & 2;
    t.last_play = m & 4;
    t.odds = m & 8;
    std::string text = status_text(s, t, (m & 16) ? "Sun 1:00 PM" : "");
    bool found = false;
    for (auto &p : seen) {
      if (p.first == text) {
        p.second += "," + std::to_string(m);
        found = true;
        break;
      }
    }
    if (!found)
      seen.emplace_back(text, std::to_string(m));
  }
  for (const auto &p : seen)
    o << "  status[" << p.second << "]=" << p.first << "\n";
}

void dump_splashes(std::ostream &o, const GameSnapshot &base) {
  GameSnapshot prev = base;
  prev.state = GameState::IN;
  for (int neutral = 0; neutral < 2; neutral++) {
    for (int opp = 0; opp < 2; opp++) {
      for (int d = -1; d <= 9; d++) {
        GameSnapshot ours = prev;
        ours.team_score += d;
        GameSnapshot theirs = prev;
        theirs.opp_score += d;
        Splash a = decide_splash(prev, ours, opp, neutral);
        Splash b = decide_splash(prev, theirs, opp, neutral);
        o << "  splash n" << neutral << " o" << opp << " d" << d << " us=" << a.text << "/" << a.color
          << " them=" << b.text << "/" << b.color << "\n";
      }
      for (int lead = -1; lead <= 1; lead++) {
        GameSnapshot fin = prev;
        fin.state = GameState::POST;
        fin.completed = true;
        fin.team_score = prev.opp_score + lead;
        GameSnapshot was = prev;
        was.team_score = fin.team_score;
        Splash w = decide_splash(was, fin, opp, neutral);
        o << "  final n" << neutral << " o" << opp << " lead" << lead << "=" << w.text << "/" << w.color << "\n";
      }
    }
  }
}

// Every (event, team) pair in a scoreboard fixture, parsed and rendered.
void dump_scoreboard(std::ostream &o, const char *path) {
  std::string json = slurp(path);
  JsonDocument filter;
  JsonObject fev = filter["events"].add<JsonObject>();
  fev["id"] = true;
  fev["competitions"][0]["competitors"][0]["id"] = true;
  JsonDocument doc;
  DeserializationError err =
      deserializeJson(doc, json, DeserializationOption::Filter(filter), DeserializationOption::NestingLimit(40));
  o << "scoreboard " << path << " err=" << err.c_str() << "\n";
  int pairs = 0;
  for (JsonObjectConst ev : doc["events"].as<JsonArrayConst>()) {
    std::string id = ev["id"].as<const char *>();
    for (JsonObjectConst c : ev["competitions"][0]["competitors"].as<JsonArrayConst>()) {
      uint32_t team = (uint32_t) atol(c["id"].as<const char *>());
      GameSnapshot s;
      bool ok = parse_scoreboard_str(json, id, team, s);
      o << " event " << id << " team " << team << " ok=" << ok << "\n";
      dump_snapshot(o, s);
      dump_text(o, s);
      // Splash words depend only on the score change; a few games cover them.
      if (pairs++ < 4)
        dump_splashes(o, s);
    }
  }
  std::vector<LiveGame> live;
  bool ok = parse_live_games(json, live);
  o << " live ok=" << ok << " n=" << live.size() << "\n";
  for (const auto &g : live)
    o << "  " << (int) g.league << " " << g.event_id << " grp=" << g.group << " away=" << g.away_id << " "
      << g.away_abbr << " @ " << g.home_abbr << "\n";
}

void dump_team(std::ostream &o, const char *path) {
  Schedule s;
  bool ok = parse_team_str(slurp(path), s);
  o << "team " << path << " ok=" << ok << " valid=" << s.valid << " league=" << (int) s.league
    << " event=" << s.event_id << " kick=" << s.kickoff_epoch << " group=" << s.group
    << " color=" << s.team_color << " rec=" << s.team_record << "\n";
}

void dump_upcoming(std::ostream &o, const char *path, uint32_t team) {
  std::vector<Upcoming> up;
  bool ok = parse_upcoming_str(slurp(path), team, 10, up);
  o << "upcoming " << path << " ok=" << ok << " n=" << up.size() << "\n";
  for (const auto &u : up)
    o << "  " << u.event_id << " " << u.kickoff_epoch << " " << u.opp_id << " " << u.opp_abbr << " "
      << u.opp_name << " home=" << u.home << " neutral=" << u.neutral << " tv=" << u.tv << "\n";
}

void dump_urls(std::ostream &o) {
  const League leagues[] = {League::NFL, League::NCAA};
  const char *times[] = {"2026-09-05T16:00Z", "2026-09-06T03:30Z", "2026-09-06T04:59Z",
                         "2026-09-06T05:00Z", "2026-11-02T01:15Z", "2027-01-01T00:00Z"};
  for (League l : leagues) {
    o << "league " << (int) l << " path=" << league_path(l) << "\n";
    o << "  team=" << team_url(l, 6) << " sched=" << schedule_url(l, 333) << "\n";
    o << "  logo=" << team_logo_url(l, 6, "DAL") << " logo2=" << team_logo_url(l, 2483, "ORE") << "\n";
    for (const char *t : times) {
      int64_t e = parse_iso8601_z(t);
      o << "  " << t << " sb=" << scoreboard_url(l, 8, e) << " scan=" << scan_url(l, e) << "\n";
    }
  }
  o << "dark=" << dark_logo("https://a.espncdn.com/i/teamlogos/nfl/500/scoreboard/dal.png") << "\n";
}

void dump_kickoff(std::ostream &o) {
  struct tm now{};
  now.tm_year = 126;
  now.tm_yday = 247;
  now.tm_wday = 6;
  now.tm_hour = 10;
  for (int days = -1; days <= 9; days++) {
    struct tm k = now;
    k.tm_yday += days;
    k.tm_wday = (6 + days + 7) % 7;
    k.tm_hour = (days * 5 + 24) % 24;
    k.tm_min = days * 7 % 60 < 0 ? 0 : days * 7 % 60;
    k.tm_mon = 8;
    k.tm_mday = 5 + days;
    o << "kickoff " << days << "=" << kickoff_label(k, now) << "\n";
  }
}

}  // namespace

// Returns the number of failures (0 or 1) so main() can count it.
int run_football_golden() {
  std::ostringstream o;
  dump_urls(o);
  dump_kickoff(o);
  dump_team(o, "fixtures/team_nfl_dal.json");
  dump_team(o, "fixtures/team_ncaa_bc.json");
  dump_upcoming(o, "fixtures/schedule_nfl_dal.json", 6);
  dump_upcoming(o, "fixtures/schedule_ncaa_bc.json", 103);
  dump_scoreboard(o, "fixtures/scoreboard_nfl.json");
  dump_scoreboard(o, "fixtures/scoreboard_ncaa_live.json");
  std::string got = o.str();
  const char *path = "fixtures/football_golden.txt";
  const char *write = getenv("GOLDEN_WRITE");
  if (write != nullptr && write[0] == '1') {
    std::ofstream f(path, std::ios::binary);
    f << got;
    printf("wrote %s (%u bytes)\n", path, (unsigned) got.size());
    return 0;
  }
  std::string want = slurp(path);
  if (got == want)
    return 0;
  size_t i = 0;
  while (i < got.size() && i < want.size() && got[i] == want[i])
    i++;
  size_t line = 1;
  for (size_t k = 0; k < i && k < want.size(); k++)
    line += want[k] == '\n';
  printf("FAIL football golden differs at line %u (byte %u)\n", (unsigned) line, (unsigned) i);
  return 1;
}
