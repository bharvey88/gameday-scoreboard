// Replays recorded single-event ESPN documents through the firmware's parse,
// text and splash code, game by game in time order, from both teams' side.
// Usage: replay <dir> <sport 1-4> [file prefix, e.g. ev_nba_]
#include <ArduinoJson.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "espn_parse.h"

using namespace espn;
namespace fs = std::filesystem;

static std::string slurp(const fs::path &p) {
  std::ifstream f(p, std::ios::binary);
  std::stringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

int main(int argc, char **argv) {
  if (argc < 3)
    return 2;
  fs::path dir = argv[1];
  Sport sport = (Sport) atoi(argv[2]);
  std::string prefix = argc > 3 ? argv[3] : "ev_";
  // event id -> files in time order (the timestamp is the name's tail)
  std::map<std::string, std::vector<fs::path>> games;
  for (auto &e : fs::directory_iterator(dir)) {
    std::string n = e.path().filename().string();
    if (n.rfind(prefix, 0) != 0 || n.size() < prefix.size() + 10)
      continue;
    std::string rest = n.substr(prefix.size());
    size_t us = rest.find('_');
    if (us == std::string::npos)
      continue;
    games[rest.substr(0, us)].push_back(e.path());
  }
  int docs = 0, fails = 0, anomalies = 0, splashes = 0, games_n = 0;
  std::map<std::string, int> words;
  auto note = [&](const std::string &s) {
    if (anomalies++ < 12)
      printf("  ! %s\n", s.c_str());
  };
  for (auto &g : games) {
    std::sort(g.second.begin(), g.second.end());
    games_n++;
    // the two teams, from the first document
    std::vector<uint32_t> sides;
    {
      JsonDocument doc;
      std::string first = slurp(g.second.front());
      if (deserializeJson(doc, first, DeserializationOption::NestingLimit(40)) == DeserializationError::Ok)
      {
        JsonArrayConst competitors = doc["competitions"][0]["competitors"];
        for (JsonObjectConst c : competitors)
          sides.push_back((uint32_t) atol(c["id"].as<const char *>()));
      }
    }
    for (uint32_t team : sides) {
      GameSnapshot prev;
      for (auto &f : g.second) {
        GameSnapshot cur;
        docs++;
        if (!parse_event_str(slurp(f), sport, team, cur)) {
          fails++;
          note("parse failed: " + f.filename().string());
          continue;
        }
        TickerOptions o;
        o.clock = false;
        if (cur.state == GameState::IN) {
          if (clock_text(cur).empty())
            note("empty clock: " + f.filename().string() + " [" + cur.short_detail + "]");
          if (status_text(cur, o, "").empty())
            note("empty ticker: " + f.filename().string());
        }
        if (prev.valid && prev.state == GameState::POST && cur.state == GameState::IN)
          note("POST -> IN: " + f.filename().string());
        Splash s = decide_splash(prev, cur, true, false);
        bool scored = prev.valid && cur.state == GameState::IN && prev.state == GameState::IN &&
                      (cur.team_score > prev.team_score || cur.opp_score > prev.opp_score);
        bool won = prev.valid && prev.state == GameState::IN && cur.state == GameState::POST &&
                   cur.team_score > cur.opp_score;
        if (!s.text.empty()) {
          splashes++;
          words[s.text]++;
          if (!scored && !won)
            note("splash without a score: " + s.text + " " + f.filename().string());
        } else if (sport != Sport::BASKETBALL && (scored || won)) {
          note("score with no splash: " + f.filename().string() + " " + std::to_string(prev.team_score) + "-" +
               std::to_string(prev.opp_score) + " -> " + std::to_string(cur.team_score) + "-" +
               std::to_string(cur.opp_score));
        }
        prev = cur;
      }
    }
  }
  printf("%s: %d games, %d parses, %d failed, %d splashes, %d anomalies\n", dir.filename().string().c_str(), games_n,
         docs, fails, splashes, anomalies);
  for (auto &w : words)
    printf("    %3d x %s\n", w.second, w.first.c_str());
  return 0;
}
