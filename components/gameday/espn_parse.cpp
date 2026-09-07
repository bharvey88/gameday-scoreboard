#include "espn_parse.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace espn {

static const char *kSite = "https://site.api.espn.com/apis/site/v2/sports/football/";

const char *league_path(League league) { return league == League::NFL ? "nfl" : "college-football"; }

std::string team_url(League league, uint32_t espn_id) {
  return std::string(kSite) + league_path(league) + "/teams/" + std::to_string(espn_id);
}

// Number of days since 1970-01-01 for a UTC epoch, then split into y/m/d.
// Avoids gmtime_r so the host and device agree regardless of libc quirks.
static void civil_from_epoch(int64_t epoch, int &y, int &m, int &d) {
  int64_t z = epoch / 86400 + 719468;
  if (epoch < 0 && epoch % 86400 != 0)
    z -= 1;
  int64_t era = (z >= 0 ? z : z - 146096) / 146097;
  int64_t doe = z - era * 146097;
  int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  int64_t yy = yoe + era * 400;
  int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  int64_t mp = (5 * doy + 2) / 153;
  d = (int) (doy - (153 * mp + 2) / 5 + 1);
  m = (int) (mp < 10 ? mp + 3 : mp - 9);
  y = (int) (yy + (m <= 2 ? 1 : 0));
}

std::string scoreboard_url(League league, uint32_t group, int64_t kickoff_epoch) {
  std::string url = std::string(kSite) + league_path(league) + "/scoreboard";
  int y, m, d;
  // ESPN's date parameter is in US Eastern; shift the UTC kickoff by five
  // hours so late east-coast games stay on the right day. Being off by an
  // hour around a DST change only matters for a game kicking off between
  // 11 PM and midnight Eastern, which does not happen in football.
  civil_from_epoch(kickoff_epoch - 5 * 3600, y, m, d);
  char buf[64];
  if (league == League::NCAA)
    snprintf(buf, sizeof(buf), "?groups=%u&dates=%04d%02d%02d", (unsigned) group, y, m, d);
  else
    snprintf(buf, sizeof(buf), "?dates=%04d%02d%02d", y, m, d);
  url += buf;
  return url;
}

std::string scan_url(League league, int64_t now_epoch) {
  std::string url = std::string(kSite) + league_path(league) + "/scoreboard";
  int y, m, d;
  civil_from_epoch(now_epoch - 5 * 3600, y, m, d);
  char buf[64];
  if (league == League::NCAA)
    snprintf(buf, sizeof(buf), "?groups=80&limit=300&dates=%04d%02d%02d", y, m, d);
  else
    snprintf(buf, sizeof(buf), "?dates=%04d%02d%02d", y, m, d);
  url += buf;
  return url;
}

// Dark-background variant, shrunk to 64px by ESPN's image resizer. A 500px
// logo PNG is up to 95KB and takes the ESP32 almost two seconds to decode;
// the 64px one is about 3KB and decodes in a few milliseconds.
std::string dark_logo(const std::string &url) {
  if (url.empty())
    return url;
  std::string out = url;
  size_t pos = out.find("/500/");
  if (pos != std::string::npos)
    out.replace(pos, 5, "/500-dark/");
  size_t path = out.find("/i/teamlogos/");
  if (path == std::string::npos)
    return out;
  return "https://a.espncdn.com/combiner/i?img=" + out.substr(path) + "&w=64&h=64";
}

std::string team_logo_url(League league, uint32_t espn_id, const char *abbr) {
  std::string a = abbr ? abbr : "";
  for (auto &c : a)
    c = (char) tolower((unsigned char) c);
  if (league == League::NFL)
    return dark_logo("https://a.espncdn.com/i/teamlogos/nfl/500/" + a + ".png");
  return dark_logo("https://a.espncdn.com/i/teamlogos/ncaa/500/" + std::to_string(espn_id) + ".png");
}

bool parse_team_str(const std::string &json, Schedule &out) { return parse_team(json, out); }

bool parse_scoreboard_str(const std::string &json, const std::string &event_id, uint32_t our_team_id,
                          GameSnapshot &out) {
  return parse_scoreboard(json, event_id, our_team_id, out);
}

uint32_t parse_color(const std::string &hex) {
  if (hex.size() != 6)
    return 0xFFFFFF;
  char *end = nullptr;
  unsigned long v = strtoul(hex.c_str(), &end, 16);
  if (end == nullptr || *end != '\0')
    return 0xFFFFFF;
  return (uint32_t) v;
}

static const char *score_word(int delta, bool ours) {
  switch (delta) {
    case 6:
    case 7:
    case 8:
      return ours ? "TOUCHDOWN!" : "TOUCHDOWN";
    case 3:
      return ours ? "FIELD GOAL!" : "FIELD GOAL";
    case 1:
      return ours ? "EXTRA POINT!" : "EXTRA POINT";
    case 2:
      return ours ? "2-POINT!" : "2-POINT";
    default:
      return ours ? "SCORE!" : "SCORE";
  }
}

Splash decide_splash(const GameSnapshot &prev, const GameSnapshot &cur, bool opponent_splashes, bool neutral) {
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
  int delta = cur.team_score - prev.team_score;
  if (delta > 0 && neutral)
    return Splash{cur.team_abbr + " " + score_word(delta, false), parse_color(cur.team_color)};
  if (delta > 0)
    return Splash{score_word(delta, true), parse_color(cur.team_color)};
  int odelta = cur.opp_score - prev.opp_score;
  if (odelta > 0 && opponent_splashes)
    return Splash{cur.opp_abbr + " " + score_word(odelta, false), parse_color(cur.opp_color)};
  return none;
}

static void add_part(std::string &out, const std::string &part) {
  if (part.empty())
    return;
  if (!out.empty())
    out += " | ";
  out += part;
}

static std::string records_line(const GameSnapshot &s) {
  std::string out;
  if (!s.team_record.empty())
    add_part(out, s.team_abbr + " " + s.team_record);
  if (!s.opp_record.empty())
    add_part(out, s.opp_abbr + " " + s.opp_record);
  return out;
}

std::string status_text(const GameSnapshot &s, const TickerOptions &o, const std::string &kickoff_local) {
  std::string out;
  switch (s.state) {
    case GameState::NOT_FOUND:
      return "No upcoming game";
    case GameState::PRE:
      add_part(out, kickoff_local.empty() ? s.short_detail : kickoff_local);
      if (o.odds) {
        add_part(out, s.odds);
        if (!s.over_under.empty())
          add_part(out, "O/U " + s.over_under);
        add_part(out, s.tv);
      }
      add_part(out, s.venue);
      return out;
    case GameState::POST:
      out = "Final";
      add_part(out, records_line(s));
      return out;
    case GameState::IN:
      break;
  }
  if (s.short_detail.find("Halftime") != std::string::npos) {
    out = "Halftime";
    add_part(out, records_line(s));
    return out;
  }
  if (o.down_distance)
    add_part(out, s.down_distance);
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

static std::string clock_12h(const struct tm &t) {
  int h = t.tm_hour % 12;
  if (h == 0)
    h = 12;
  char buf[16];
  snprintf(buf, sizeof(buf), "%d:%02d %s", h, t.tm_min, t.tm_hour < 12 ? "AM" : "PM");
  return buf;
}

std::string kickoff_label(const struct tm &kick_local, const struct tm &now_local) {
  static const char *kDays[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
  static const char *kMonths[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                  "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
  int now_day = now_local.tm_year * 366 + now_local.tm_yday;
  int kick_day = kick_local.tm_year * 366 + kick_local.tm_yday;
  int diff = kick_day - now_day;
  std::string when;
  if (diff == 0)
    when = "Today";
  else if (diff == 1)
    when = "Tomorrow";
  else if (diff > 1 && diff < 7)
    when = kDays[kick_local.tm_wday % 7];
  else {
    char buf[16];
    snprintf(buf, sizeof(buf), "%s %d", kMonths[kick_local.tm_mon % 12], kick_local.tm_mday);
    when = buf;
  }
  return when + " " + clock_12h(kick_local);
}

// "2026-09-05T19:30Z" or "2026-09-05T19:30:00Z" (ESPN always sends UTC).
int64_t parse_iso8601_z(const std::string &s) {
  int y = 0, mo = 0, d = 0, h = 0, mi = 0, sec = 0;
  int n = sscanf(s.c_str(), "%d-%d-%dT%d:%d:%d", &y, &mo, &d, &h, &mi, &sec);
  if (n < 5)
    return 0;
  // days from civil (Howard Hinnant's algorithm)
  int yy = y - (mo <= 2 ? 1 : 0);
  int64_t era = (yy >= 0 ? yy : yy - 399) / 400;
  int64_t yoe = yy - era * 400;
  int64_t doy = (153 * (mo + (mo > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  int64_t days = era * 146097 + doe - 719468;
  return days * 86400 + h * 3600 + mi * 60 + sec;
}

const char *state_name(GameState s) {
  switch (s) {
    case GameState::PRE:
      return "PRE";
    case GameState::IN:
      return "IN";
    case GameState::POST:
      return "POST";
    default:
      return "NOT_FOUND";
  }
}

}  // namespace espn
