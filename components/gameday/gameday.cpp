#include "gameday.h"

#include "esphome/components/network/util.h"
#include "esphome/core/application.h"
#include "esphome/core/hal.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"

#include <cstring>
#include <vector>

#include "timezones.h"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

namespace esphome {
namespace gameday {

static const char *const TAG = "gameday";

// ESPN's edge returns 403 to most unfamiliar agents but accepts curl.
static const char *const USER_AGENT = "curl/8.0 gameday-scoreboard";

static const uint32_t MINUTE = 60 * 1000;
static const uint32_t SCHEDULE_INTERVAL = 6 * 60 * MINUTE;
static const uint32_t PRE_FAR_INTERVAL = 15 * MINUTE;
static const uint32_t PRE_NEAR_INTERVAL = MINUTE;
static const uint32_t IN_INTERVAL = 5 * 1000;
static const uint32_t POST_INTERVAL = MINUTE;
static const uint32_t POST_LINGER = 30 * MINUTE;
static const uint32_t RETRY_INTERVAL = MINUTE;
static const uint32_t NOT_READY_INTERVAL = 5 * 1000;
static const int64_t PRE_NEAR_SECONDS = 60 * 60;
static const uint32_t NO_LIVE_RESCAN = 2 * MINUTE;

static const char *const MODE_OPTIONS[] = {"My team", "Live NFL", "Live college", "Live anything"};

// Feeds ArduinoJson straight from the HTTP socket so a scoreboard document
// never has to sit in RAM as a whole.
class ContainerReader {
 public:
  explicit ContainerReader(std::shared_ptr<http_request::HttpContainer> container)
      : container_(std::move(container)) {}

  int read() {
    if (!this->fill_())
      return -1;
    return static_cast<unsigned char>(this->buf_[this->pos_++]);
  }

  size_t readBytes(char *dst, size_t n) {
    size_t copied = 0;
    while (copied < n) {
      if (!this->fill_())
        break;
      size_t take = std::min(n - copied, this->len_ - this->pos_);
      memcpy(dst + copied, this->buf_ + this->pos_, take);
      this->pos_ += take;
      copied += take;
    }
    return copied;
  }

  size_t total() const { return this->total_; }

 private:
  bool fill_() {
    if (this->pos_ < this->len_)
      return true;
    if (this->eof_)
      return false;
    int n = this->container_->read(this->buf_, sizeof(this->buf_));
    if (n <= 0) {
      this->eof_ = true;
      return false;
    }
    this->pos_ = 0;
    this->len_ = (size_t) n;
    this->total_ += (size_t) n;
    return true;
  }

  std::shared_ptr<http_request::HttpContainer> container_;
  uint8_t buf_[1024];
  size_t pos_{0};
  size_t len_{0};
  size_t total_{0};
  bool eof_{false};
};

// ---- GamedaySelect ---------------------------------------------------------

void GamedaySelect::control(const std::string &value) {
  this->publish_state(value);
  if (this->parent_ == nullptr)
    return;
  if (this->type_ == SelectType::TEAM)
    this->parent_->select_team(value);
  else if (this->type_ == SelectType::MODE)
    this->parent_->select_mode(value);
  else if (this->type_ == SelectType::FAVORITE)
    this->parent_->select_favorite(this->slot_, value);
  else
    this->parent_->select_timezone(value);
}

// ---- GamedayComponent ------------------------------------------------------

void GamedayComponent::setup() {
  this->pref_ = global_preferences->make_preference<Prefs>(fnv1_hash("gameday_prefs_v1"));
  if (!this->pref_.load(&this->prefs_) || this->current_team_() == nullptr) {
    this->prefs_.league = (uint8_t) League::NFL;
    this->prefs_.team_id = 6;  // Dallas Cowboys
    this->prefs_.tz_index = ::espn::kDefaultTimezone;
    this->prefs_.flags = FLAGS_DEFAULT;
  }
  if (this->prefs_.tz_index >= ::espn::kTimezoneCount)
    this->prefs_.tz_index = ::espn::kDefaultTimezone;
  this->pref2_ = global_preferences->make_preference<Prefs2>(fnv1_hash("gameday_prefs2_v1"));
  if (!this->pref2_.load(&this->prefs2_) || this->prefs2_.mode > (uint8_t) Mode::LIVE_ANY ||
      this->prefs2_.rotate_minutes < 2 || this->prefs2_.rotate_minutes > 30) {
    this->prefs2_.mode = (uint8_t) Mode::MY_TEAM;
    this->prefs2_.rotate_minutes = 5;
  }
  this->pref3_ = global_preferences->make_preference<Prefs3>(fnv1_hash("gameday_prefs3_v1"));
  if (!this->pref3_.load(&this->prefs3_))
    this->prefs3_ = Prefs3{};
  this->apply_timezone_();
  this->publish_selects_();
  this->schedule_next_(3000);
}

std::string GamedayComponent::favorite_option_(uint8_t slot) const {
  if (slot < 1 || slot > 4 || this->prefs3_.fav_id[slot - 1] == 0)
    return "None";
  for (size_t i = 0; i < ::espn::kTeamCount; i++) {
    const auto &t = ::espn::kTeams[i];
    if ((uint8_t) t.league == this->prefs3_.fav_league[slot - 1] && t.espn_id == this->prefs3_.fav_id[slot - 1])
      return std::string(t.league == League::NFL ? "NFL: " : "NCAAF: ") + t.name;
  }
  return "None";
}

void GamedayComponent::select_favorite(uint8_t slot, const std::string &option) {
  if (slot < 1 || slot > 4)
    return;
  uint8_t league = 0;
  uint32_t id = 0;
  if (option != "None") {
    for (size_t i = 0; i < ::espn::kTeamCount; i++) {
      const auto &t = ::espn::kTeams[i];
      if (std::string(t.league == League::NFL ? "NFL: " : "NCAAF: ") + t.name == option) {
        league = (uint8_t) t.league;
        id = t.espn_id;
        break;
      }
    }
    if (id == 0) {
      ESP_LOGW(TAG, "Unknown favorite '%s'", option.c_str());
      return;
    }
  }
  this->prefs3_.fav_league[slot - 1] = league;
  this->prefs3_.fav_id[slot - 1] = id;
  this->pref3_.save(&this->prefs3_);
  ESP_LOGI(TAG, "Favorite %u: %s", (unsigned) slot, option.c_str());
}

void GamedayComponent::press_favorite(uint8_t slot) {
  std::string option = this->favorite_option_(slot);
  if (option == "None") {
    ESP_LOGI(TAG, "Favorite %u is empty", (unsigned) slot);
    return;
  }
  ESP_LOGI(TAG, "Remote button %u: %s", (unsigned) slot, option.c_str());
  if (this->prefs2_.mode != (uint8_t) Mode::MY_TEAM) {
    this->prefs2_.mode = (uint8_t) Mode::MY_TEAM;
    this->pref2_.save(&this->prefs2_);
    if (this->mode_select_ != nullptr)
      this->mode_select_->publish_state(MODE_OPTIONS[0]);
    this->reset_game_();
    this->generation_++;
    this->schedule_next_(0);
  }
  if (this->team_select_ != nullptr)
    this->team_select_->publish_state(option);
  this->select_team(option);
}

// Push the stored choices into the two selects. Runs at setup and once more
// after the network is up, so the web page and the API always see them.
void GamedayComponent::publish_selects_() {
  std::string team = this->team_option_();
  ESP_LOGI(TAG, "Publishing selects: team '%s', timezone '%s'", team.c_str(),
           ::espn::kTimezones[this->prefs_.tz_index].name);
  if (this->team_select_ != nullptr)
    this->team_select_->publish_state(team);
  if (this->timezone_select_ != nullptr)
    this->timezone_select_->publish_state(::espn::kTimezones[this->prefs_.tz_index].name);
  if (this->mode_select_ != nullptr)
    this->mode_select_->publish_state(MODE_OPTIONS[this->prefs2_.mode]);
  for (uint8_t slot = 1; slot <= 4; slot++)
    if (this->favorite_selects_[slot - 1] != nullptr)
      this->favorite_selects_[slot - 1]->publish_state(this->favorite_option_(slot));
}

void GamedayComponent::select_mode(const std::string &option) {
  for (uint8_t i = 0; i < 4; i++) {
    if (option != MODE_OPTIONS[i])
      continue;
    if (i == this->prefs2_.mode)
      return;
    this->prefs2_.mode = i;
    this->pref2_.save(&this->prefs2_);
    ESP_LOGI(TAG, "Mode: %s", option.c_str());
    this->reset_game_();
    this->generation_++;
    this->schedule_next_(0);
    return;
  }
  ESP_LOGW(TAG, "Unknown mode '%s'", option.c_str());
}

void GamedayComponent::set_rotate_minutes(int minutes) {
  if (minutes < 2)
    minutes = 2;
  if (minutes > 30)
    minutes = 30;
  if (minutes == this->prefs2_.rotate_minutes)
    return;
  this->prefs2_.rotate_minutes = (uint8_t) minutes;
  this->pref2_.save(&this->prefs2_);
}

uint32_t GamedayComponent::our_id_() const {
  if (this->live_mode_())
    return this->live_away_id_;
  const ::espn::Team *t = this->current_team_();
  return t ? t->espn_id : 0;
}

void GamedayComponent::dump_config() {
  ESP_LOGCONFIG(TAG, "Game Day Scoreboard:");
  const ::espn::Team *t = this->current_team_();
  ESP_LOGCONFIG(TAG, "  Team: %s (%s id %u)", t ? t->name : "?", t ? t->abbr : "?", (unsigned) this->prefs_.team_id);
  ESP_LOGCONFIG(TAG, "  Timezone: %s", ::espn::kTimezones[this->prefs_.tz_index].name);
  ESP_LOGCONFIG(TAG, "  Flags: 0x%02X", this->prefs_.flags);
}

void GamedayComponent::loop() {
  if (this->busy_) {
    if (!this->job_done_)
      return;
    this->busy_ = false;
    this->job_done_ = false;
    this->apply_job_();
    return;
  }
  if ((int32_t) (millis() - this->next_fetch_ms_) < 0)
    return;
  if (!network::is_connected() || this->time_ == nullptr || !this->time_->now().is_valid()) {
    this->schedule_next_(NOT_READY_INTERVAL);
    return;
  }
  if (!this->selects_published_) {
    this->selects_published_ = true;
    this->publish_selects_();
  }
  this->start_job_();
}

const ::espn::Team *GamedayComponent::current_team_() const {
  for (size_t i = 0; i < ::espn::kTeamCount; i++) {
    const auto &t = ::espn::kTeams[i];
    if ((uint8_t) t.league == this->prefs_.league && t.espn_id == this->prefs_.team_id)
      return &t;
  }
  return nullptr;
}

std::string GamedayComponent::team_option_() const {
  const ::espn::Team *t = this->current_team_();
  if (t == nullptr)
    return "";
  return std::string(t->league == League::NFL ? "NFL: " : "NCAAF: ") + t->name;
}

void GamedayComponent::select_team(const std::string &option) {
  for (size_t i = 0; i < ::espn::kTeamCount; i++) {
    const auto &t = ::espn::kTeams[i];
    std::string name = std::string(t.league == League::NFL ? "NFL: " : "NCAAF: ") + t.name;
    if (name != option)
      continue;
    if ((uint8_t) t.league == this->prefs_.league && t.espn_id == this->prefs_.team_id)
      return;
    this->prefs_.league = (uint8_t) t.league;
    this->prefs_.team_id = t.espn_id;
    this->save_prefs_();
    ESP_LOGI(TAG, "Team changed to %s", t.name);
    this->reset_game_();
    this->generation_++;  // a fetch already in flight belongs to the old team
    this->schedule_next_(0);
    return;
  }
  ESP_LOGW(TAG, "Unknown team option '%s'", option.c_str());
}

void GamedayComponent::select_timezone(const std::string &option) {
  for (size_t i = 0; i < ::espn::kTimezoneCount; i++) {
    if (option != ::espn::kTimezones[i].name)
      continue;
    this->prefs_.tz_index = (uint8_t) i;
    this->save_prefs_();
    this->apply_timezone_();
    // Re-render so a PRE ticker picks up the new kickoff time.
    if (this->game_.valid)
      this->emit_({});
    return;
  }
  ESP_LOGW(TAG, "Unknown timezone option '%s'", option.c_str());
}

void GamedayComponent::refresh_now() {
  this->schedule_fetched_ms_ = 0;
  this->schedule_next_(0);
}

void GamedayComponent::set_flag_(uint8_t f, bool on) {
  uint8_t next = on ? (this->prefs_.flags | f) : (this->prefs_.flags & ~f);
  if (next == this->prefs_.flags)
    return;
  this->prefs_.flags = next;
  this->save_prefs_();
  if (this->game_.valid)
    this->emit_({});
}

void GamedayComponent::save_prefs_() { this->pref_.save(&this->prefs_); }

void GamedayComponent::apply_timezone_() {
  if (this->time_ != nullptr)
    this->time_->set_timezone(::espn::kTimezones[this->prefs_.tz_index].posix);
}

void GamedayComponent::reset_game_() {
  this->live_away_id_ = 0;
  this->live_started_ms_ = 0;
  this->live_none_ = false;
  this->schedule_ = Schedule{};
  this->schedule_fetched_ms_ = 0;
  this->game_ = GameSnapshot{};
  this->prev_ = GameSnapshot{};
  this->post_since_ms_ = 0;
  this->misses_ = 0;
}

std::shared_ptr<http_request::HttpContainer> GamedayComponent::open_(const std::string &url) {
  std::vector<http_request::Header> headers = {
      {"User-Agent", USER_AGENT},
      {"Accept", "application/json"},
  };
  auto container = this->http_->get(url, headers);
  if (container == nullptr) {
    ESP_LOGW(TAG, "Request failed: %s", url.c_str());
    return nullptr;
  }
  if (container->status_code != 200) {
    ESP_LOGW(TAG, "HTTP %d for %s", container->status_code, url.c_str());
    container->end();
    return nullptr;
  }
  return container;
}

bool GamedayComponent::fetch_schedule_(const ::espn::Team *team, Schedule &out) {
  std::string url = ::espn::team_url(team->league, team->espn_id);
  ESP_LOGD(TAG, "Fetching schedule: %s", url.c_str());
  auto container = this->open_(url);
  if (container == nullptr)
    return false;
  ContainerReader reader(container);
  Schedule s;
  bool ok = ::espn::parse_team(reader, s);
  container->end();
  if (!ok) {
    ESP_LOGW(TAG, "Schedule parse failed after %u bytes", (unsigned) reader.total());
    return false;
  }
  ESP_LOGI(TAG, "Schedule: event %s kickoff %lld group %u (%u bytes)", s.event_id.c_str(),
           (long long) s.kickoff_epoch, (unsigned) s.group, (unsigned) reader.total());
  out = s;
  return true;
}

bool GamedayComponent::fetch_live_games_(League league, std::vector<::espn::LiveGame> &out) {
  std::string url = ::espn::scan_url(league, (int64_t) this->time_->timestamp_now());
  ESP_LOGD(TAG, "Scanning for live games: %s", url.c_str());
  auto container = this->open_(url);
  if (container == nullptr)
    return false;
  ContainerReader reader(container);
  size_t before = out.size();
  bool ok = ::espn::parse_live_games(reader, out);
  for (size_t i = before; i < out.size(); i++)
    out[i].league = (uint8_t) league;
  container->end();
  ESP_LOGI(TAG, "Scan: %u live game(s) (%u bytes)", (unsigned) out.size(), (unsigned) reader.total());
  return ok;
}

bool GamedayComponent::fetch_game_(const ::espn::Team *team, const Schedule &schedule, GameSnapshot &out) {
  League league = this->live_mode_() ? (League) schedule.league : team->league;
  std::string url = ::espn::scoreboard_url(league, schedule.group, schedule.kickoff_epoch);
  ESP_LOGD(TAG, "Fetching game: %s", url.c_str());
  auto container = this->open_(url);
  if (container == nullptr)
    return false;
  ContainerReader reader(container);
  GameSnapshot g;
  bool ok = ::espn::parse_scoreboard(reader, schedule.event_id, this->our_id_(), g);
  container->end();
  if (!ok) {
    ESP_LOGW(TAG, "Game %s not parsed from scoreboard (%u bytes)", schedule.event_id.c_str(),
             (unsigned) reader.total());
    return false;
  }
  ESP_LOGI(TAG, "Game: %s %s %d - %s %d [%s] (%u bytes)", ::espn::state_name(g.state), g.team_abbr.c_str(),
           g.team_score, g.opp_abbr.c_str(), g.opp_score, g.short_detail.c_str(), (unsigned) reader.total());
  out = g;
  return true;
}

uint32_t GamedayComponent::interval_for_phase_() const {
  switch (this->game_.state) {
    case GameState::IN:
      return IN_INTERVAL;
    case GameState::POST:
      return POST_INTERVAL;
    case GameState::PRE: {
      int64_t now = (int64_t) this->time_->timestamp_now();
      int64_t until = this->game_.kickoff_epoch - now;
      return until <= PRE_NEAR_SECONDS ? PRE_NEAR_INTERVAL : PRE_FAR_INTERVAL;
    }
    default:
      return SCHEDULE_INTERVAL;
  }
}

// Main loop: decide what this cycle needs and hand it to the worker task.
void GamedayComponent::start_job_() {
  uint32_t now = millis();
  Job &j = this->job_;
  j = Job{};
  j.generation = this->generation_;
  j.team = this->current_team_();
  if (j.team == nullptr) {
    this->schedule_next_(SCHEDULE_INTERVAL);
    return;
  }
  if (this->live_mode_()) {
    bool rotate = this->live_started_ms_ != 0 &&
                  (now - this->live_started_ms_) >= (uint32_t) this->prefs2_.rotate_minutes * MINUTE;
    bool ended = this->game_.valid && this->game_.state != GameState::IN;
    j.need_scan = !this->schedule_.valid || rotate || ended || this->live_none_;
    j.schedule = this->schedule_;
    this->job_done_ = false;
    this->busy_ = true;
    BaseType_t ok = xTaskCreate(&GamedayComponent::worker_, "gameday_fetch", 16384, this, 1, nullptr);
    if (ok != pdPASS) {
      this->busy_ = false;
      this->schedule_next_(RETRY_INTERVAL);
    }
    return;
  }
  j.need_schedule = !this->schedule_.valid || (now - this->schedule_fetched_ms_) >= SCHEDULE_INTERVAL;
  if (this->post_since_ms_ != 0 && (now - this->post_since_ms_) >= POST_LINGER) {
    // Game is over and lingered: look for the next one.
    this->prev_ = GameSnapshot{};
    this->game_ = GameSnapshot{};
    this->post_since_ms_ = 0;
    j.need_schedule = true;
  }
  j.schedule = this->schedule_;
  this->job_done_ = false;
  this->busy_ = true;
  // TLS plus a nested JSON parse needs a roomy stack; 16KB has headroom.
  BaseType_t ok = xTaskCreate(&GamedayComponent::worker_, "gameday_fetch", 16384, this, 1, nullptr);
  if (ok != pdPASS) {
    ESP_LOGW(TAG, "Could not start the fetch task");
    this->busy_ = false;
    this->schedule_next_(RETRY_INTERVAL);
  }
}

void GamedayComponent::worker_(void *arg) {
  auto *self = static_cast<GamedayComponent *>(arg);
  self->run_job_();
  self->job_done_ = true;
  vTaskDelete(nullptr);
}

// Worker task: network and parsing only, no display or entity access.
void GamedayComponent::run_job_() {
  Job &j = this->job_;
  if (j.need_scan) {
    uint8_t mode = this->prefs2_.mode;
    j.scan_ok = true;
    if (mode == (uint8_t) Mode::LIVE_NFL || mode == (uint8_t) Mode::LIVE_ANY)
      j.scan_ok = this->fetch_live_games_(League::NFL, j.live) && j.scan_ok;
    if (mode == (uint8_t) Mode::LIVE_NCAA || mode == (uint8_t) Mode::LIVE_ANY)
      j.scan_ok = this->fetch_live_games_(League::NCAA, j.live) && j.scan_ok;
    return;  // the main loop picks a game, then the next cycle polls it
  }
  if (j.need_schedule) {
    Schedule s;
    j.schedule_ok = this->fetch_schedule_(j.team, s);
    if (!j.schedule_ok)
      return;
    j.schedule = s;
    if (s.event_id.empty()) {
      j.no_event = true;
      return;
    }
  }
  j.game_ok = this->fetch_game_(j.team, j.schedule, j.game);
}

// Main loop: fold the worker's result into the component state and render.
void GamedayComponent::apply_job_() {
  Job &j = this->job_;
  if (j.generation != this->generation_) {
    ESP_LOGD(TAG, "Discarding a fetch for the previous team");
    return;
  }
  uint32_t now = millis();
  if (j.need_scan) {
    if (!j.scan_ok && j.live.empty()) {
      this->misses_++;
      this->emit_({});
      this->schedule_next_(RETRY_INTERVAL);
      return;
    }
    // Prefer a different game than the one we just showed.
    std::vector<size_t> pick;
    for (size_t i = 0; i < j.live.size(); i++)
      if (j.live[i].event_id != this->schedule_.event_id)
        pick.push_back(i);
    if (pick.empty() && !j.live.empty())
      pick.push_back(0);
    if (pick.empty()) {
      ESP_LOGI(TAG, "No live games right now");
      this->live_none_ = true;
      this->schedule_ = Schedule{};
      this->game_ = GameSnapshot{};
      this->prev_ = GameSnapshot{};
      this->misses_ = 0;
      this->emit_({});
      this->schedule_next_(NO_LIVE_RESCAN);
      return;
    }
    const ::espn::LiveGame &g = j.live[pick[random_uint32() % pick.size()]];
    ESP_LOGI(TAG, "Following %s @ %s", g.away_abbr.c_str(), g.home_abbr.c_str());
    this->live_none_ = false;
    this->schedule_ = Schedule{};
    this->schedule_.valid = true;
    this->schedule_.event_id = g.event_id;
    this->schedule_.group = g.group;
    this->schedule_.league = g.league;
    this->schedule_.kickoff_epoch = (int64_t) this->time_->timestamp_now();
    this->schedule_fetched_ms_ = now;
    this->live_away_id_ = g.away_id;
    this->live_started_ms_ = now == 0 ? 1 : now;
    this->game_ = GameSnapshot{};
    this->prev_ = GameSnapshot{};
    this->schedule_next_(0);  // poll the chosen game right away
    return;
  }
  if (j.need_schedule) {
    if (!j.schedule_ok) {
      this->misses_++;
      this->emit_({});
      this->schedule_next_(RETRY_INTERVAL);
      return;
    }
    this->schedule_ = j.schedule;
    this->schedule_fetched_ms_ = now;
    if (j.no_event) {
      ESP_LOGI(TAG, "No upcoming game for this team");
      this->game_ = GameSnapshot{};
      this->misses_ = 0;
      this->emit_({});
      this->schedule_next_(SCHEDULE_INTERVAL);
      return;
    }
  }
  if (!j.game_ok) {
    this->misses_++;
    this->emit_({});
    this->schedule_next_(RETRY_INTERVAL);
    return;
  }
  this->prev_ = this->game_;
  this->game_ = j.game;
  this->misses_ = 0;
  ::espn::Splash splash =
      ::espn::decide_splash(this->prev_, this->game_, this->opponent_splashes(), this->live_mode_());
  if (this->game_.state == GameState::POST) {
    if (this->post_since_ms_ == 0)
      this->post_since_ms_ = now == 0 ? 1 : now;
  } else {
    this->post_since_ms_ = 0;
  }
  this->emit_(splash);
  this->schedule_next_(this->interval_for_phase_());
}

void GamedayComponent::emit_(const ::espn::Splash &splash) {
  const GameSnapshot &g = this->game_;
  UpdateFields f;
  f.game_state = g.valid ? ::espn::state_name(g.state) : "NOT_FOUND";
  f.team_abbr = g.team_abbr;
  f.team_score = g.team_score;
  f.opponent_abbr = g.opp_abbr;
  f.opponent_score = g.opp_score;
  f.team_logo = g.team_logo;
  f.opponent_logo = g.opp_logo;
  f.possession = g.possession;
  f.team_timeouts = g.team_timeouts;
  f.opponent_timeouts = g.opp_timeouts;
  f.team_record = g.team_record;
  f.opponent_record = g.opp_record;
  f.last_play = g.last_play;
  if (g.valid && g.state == GameState::IN) {
    // "12:34 - 2nd" is too wide for the middle column; "12:34 2nd" fits.
    f.clock_text = g.short_detail;
    size_t dash = f.clock_text.find(" - ");
    if (dash != std::string::npos)
      f.clock_text.replace(dash, 3, " ");
    f.down_distance = g.short_down_distance;
    f.is_red_zone = g.is_red_zone;
  }
  f.team_color = ::espn::parse_color(g.team_color);
  f.opponent_color = ::espn::parse_color(g.opp_color);
  f.splash_text = splash.text;
  f.splash_color = splash.color;

  TickerOptions opts;
  opts.clock = false;  // the clock has its own row on the panel; never in the ticker
  opts.down_distance = this->ticker_down_distance();
  opts.last_play = this->ticker_last_play();
  opts.odds = this->ticker_odds();

  std::string kickoff;
  if (g.valid && g.state == GameState::PRE && g.kickoff_epoch > 0 && this->time_ != nullptr) {
    struct tm kick = ESPTime::from_epoch_local((time_t) g.kickoff_epoch).to_c_tm();
    struct tm now = this->time_->now().to_c_tm();
    kickoff = ::espn::kickoff_label(kick, now);
  }
  f.status_text = ::espn::status_text(g, opts, kickoff);
  if (this->live_mode_() && (!g.valid || g.state == GameState::NOT_FOUND))
    f.status_text = this->live_none_ ? "No live games right now" : "Looking for a live game";
  if (this->misses_ >= 3)
    f.status_text += " *";

  // Compact JSON for the web page. Kept under 255 bytes so Home Assistant
  // accepts it as a text sensor state too.
  {
    const ::espn::Team *team = this->current_team_();
    char buf[320];
    snprintf(buf, sizeof(buf),
             "{\"s\":\"%s\",\"l\":\"%s\",\"ta\":\"%s\",\"ti\":%u,\"ts\":%d,\"oa\":\"%s\",\"oi\":%u,\"os\":%d,"
             "\"tr\":\"%s\",\"or\":\"%s\",\"c\":\"%s\",\"d\":\"%s\",\"p\":%d,\"tt\":%d,\"ot\":%d,"
             "\"tc\":\"%06X\",\"oc\":\"%06X\",\"rz\":%d,\"tv\":\"%s\",\"k\":\"%s\",\"m\":%d}",
             f.game_state.c_str(), team && team->league == League::NFL ? "nfl" : "ncaa", g.team_abbr.c_str(),
             (unsigned) g.team_id, g.team_score, g.opp_abbr.c_str(), (unsigned) g.opp_id, g.opp_score,
             g.team_record.c_str(), g.opp_record.c_str(), f.clock_text.c_str(), f.down_distance.c_str(), g.possession,
             g.team_timeouts, g.opp_timeouts, (unsigned) f.team_color, (unsigned) f.opponent_color, f.is_red_zone ? 1 : 0,
             g.tv.c_str(), kickoff.c_str(), (int) this->misses_);
    // mode and rotation, appended for the page
    size_t n = strlen(buf);
    if (n > 1 && n < sizeof(buf) - 24)
      snprintf(buf + n - 1, sizeof(buf) - n + 1, ",\"md\":%u,\"rm\":%u}", (unsigned) this->prefs2_.mode,
               (unsigned) this->prefs2_.rotate_minutes);
    f.json = buf;
  }

  if (!f.splash_text.empty())
    ESP_LOGI(TAG, "Splash: %s", f.splash_text.c_str());
  for (auto &cb : this->callbacks_)
    cb(f);
}

}  // namespace gameday
}  // namespace esphome
