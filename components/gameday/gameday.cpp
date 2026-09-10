#include "gameday.h"

#include "esphome/components/json/json_util.h"
#include "esphome/components/network/util.h"
#include "esphome/core/application.h"
#include "esphome/core/hal.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"

#include <cstring>
#include <vector>

#include "timezones.h"
#include "timezones_parsed.h"

#ifdef GAMEDAY_TZ_PARSED
#include "esphome/components/time/posix_tz.h"
#endif

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

static const char *const MODE_OPTIONS[] = {"My team", "Live NFL", "Live college", "Live anything", "Favorite teams"};
static const uint32_t FAV_TICK = 1000;  // playlist / lock decisions re-run this often

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
  if (!this->pref2_.load(&this->prefs2_) || this->prefs2_.mode > (uint8_t) Mode::FAVORITES ||
      this->prefs2_.rotate_minutes < 2 || this->prefs2_.rotate_minutes > 30) {
    this->prefs2_.mode = (uint8_t) Mode::MY_TEAM;
    this->prefs2_.rotate_minutes = 5;
  }
  this->pref3_ = global_preferences->make_preference<Prefs3>(fnv1_hash("gameday_prefs3_v1"));
  if (!this->pref3_.load(&this->prefs3_))
    this->prefs3_ = Prefs3{};
  this->pref4_ = global_preferences->make_preference<Prefs4>(fnv1_hash("gameday_prefs4_v1"));
  if (!this->pref4_.load(&this->prefs4_) || this->prefs4_.lockon_minutes < 5 || this->prefs4_.lockon_minutes > 120 ||
      this->prefs4_.release_seconds < 30 || this->prefs4_.release_seconds > 3600 || this->prefs4_.collide > 1) {
    this->prefs4_.lockon_minutes = 15;
    this->prefs4_.release_seconds = 30;
    this->prefs4_.collide = 0;
  }
  this->rebuild_favorites_(false);
  this->apply_timezone_();
  this->publish_selects_();
  this->rebuild_state_(nullptr);
  if (this->base_ != nullptr)
    this->base_->add_handler(this);
  this->schedule_next_(3000);
}

std::string GamedayComponent::hostname() const { return std::string(App.get_name().c_str()); }

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
  this->set_favorite_(slot, league, id);
}

void GamedayComponent::set_favorite_(uint8_t slot, uint8_t league, uint32_t id) {
  if (slot < 1 || slot > 4)
    return;
  this->prefs3_.fav_league[slot - 1] = league;
  this->prefs3_.fav_id[slot - 1] = id;
  this->pref3_.save(&this->prefs3_);
  ESP_LOGI(TAG, "Favorite %u: %s", (unsigned) slot, this->favorite_option_(slot).c_str());
  if (this->favorite_selects_[slot - 1] != nullptr)
    this->favorite_selects_[slot - 1]->publish_state(this->favorite_option_(slot));
  bool was_fav_mode = this->favorites_mode_();
  this->rebuild_favorites_(true);
  if (this->prefs2_.mode == (uint8_t) Mode::FAVORITES && (was_fav_mode || this->favorites_mode_())) {
    // The list changed under the mode: drop the board and let the next
    // cycle refresh what is missing and re-pick.
    this->generation_++;
    this->game_ = GameSnapshot{};
    this->prev_ = GameSnapshot{};
    this->emit_({});
    this->schedule_next_(0);
    return;
  }
  this->rebuild_state_(&this->last_fields_);
}

void GamedayComponent::press_favorite(uint8_t slot) {
  std::string option = this->favorite_option_(slot);
  if (option == "None") {
    ESP_LOGI(TAG, "Favorite %u is empty", (unsigned) slot);
    return;
  }
  ESP_LOGI(TAG, "Remote button %u: %s", (unsigned) slot, option.c_str());
  if (this->favorites_mode_()) {
    // Jump to that favorite and pin it: a live game holds, a card seeds the playlist.
    for (size_t i = 0; i < this->fav_.size(); i++) {
      if (this->fav_[i].slot != slot)
        continue;
      this->fav_pinned_ = (int) i;
      this->fav_shown_ = (int) i;
      this->fav_shown_since_ = (int64_t) this->time_->timestamp_now();
      this->fav_poll_ms_ = 0;
      this->prev_ = GameSnapshot{};
      this->game_ = this->fav_[i].game;
      this->post_since_ms_ = 0;
      this->emit_({});
      this->schedule_next_(0);
      return;
    }
    return;
  }
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
  for (uint8_t i = 0; i <= (uint8_t) Mode::FAVORITES; i++) {
    if (option != MODE_OPTIONS[i])
      continue;
    if (i == this->prefs2_.mode)
      return;
    this->prefs2_.mode = i;
    this->pref2_.save(&this->prefs2_);
    ESP_LOGI(TAG, "Mode: %s", option.c_str());
    if (this->mode_select_ != nullptr)
      this->mode_select_->publish_state(option);
    this->reset_game_();
    this->generation_++;
    this->emit_({});  // clear the board while the next fetch runs
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
  this->rebuild_state_(&this->last_fields_);
}

void GamedayComponent::set_lockon_minutes(int minutes) {
  minutes = minutes < 5 ? 5 : minutes > 120 ? 120 : minutes;
  if (minutes == this->prefs4_.lockon_minutes)
    return;
  this->prefs4_.lockon_minutes = (uint8_t) minutes;
  this->pref4_.save(&this->prefs4_);
  this->rebuild_state_(&this->last_fields_);
}

void GamedayComponent::set_release_seconds(int seconds) {
  seconds = seconds < 30 ? 30 : seconds > 3600 ? 3600 : seconds;
  if (seconds == this->prefs4_.release_seconds)
    return;
  this->prefs4_.release_seconds = (uint16_t) seconds;
  this->pref4_.save(&this->prefs4_);
  this->rebuild_state_(&this->last_fields_);
}

void GamedayComponent::set_collision_alternate(bool alternate) {
  if ((alternate ? 1 : 0) == this->prefs4_.collide)
    return;
  this->prefs4_.collide = alternate ? 1 : 0;
  this->pref4_.save(&this->prefs4_);
  this->rebuild_state_(&this->last_fields_);
}

// ---- Favorite Teams mode ------------------------------------------------------

// Rebuilds the entry list from the four slots. keep_cards carries a slot's
// fetched game over when the same team is still in the list (reordering).
void GamedayComponent::rebuild_favorites_(bool keep_cards) {
  std::vector<FavEntry> old = std::move(this->fav_);
  this->fav_.clear();
  for (uint8_t i = 0; i < 4; i++) {
    if (this->prefs3_.fav_id[i] == 0)
      continue;
    const ::espn::Team *team = nullptr;
    for (size_t t = 0; t < ::espn::kTeamCount; t++) {
      if ((uint8_t) ::espn::kTeams[t].league == this->prefs3_.fav_league[i] &&
          ::espn::kTeams[t].espn_id == this->prefs3_.fav_id[i]) {
        team = &::espn::kTeams[t];
        break;
      }
    }
    if (team == nullptr)
      continue;
    FavEntry e;
    e.team = team;
    e.slot = i + 1;
    if (keep_cards) {
      for (auto &o : old) {
        if (o.team == team) {
          e = o;
          e.slot = i + 1;
          break;
        }
      }
    }
    this->fav_.push_back(e);
  }
  this->fav_shown_ = -1;
  this->fav_pinned_ = -1;
  this->fav_locked_ = false;
  this->fav_poll_ms_ = 0;
}

::espn::FavRules GamedayComponent::fav_rules_() const {
  ::espn::FavRules r;
  r.lockon_s = (int64_t) this->prefs4_.lockon_minutes * 60;
  r.release_s = this->prefs4_.release_seconds;
  r.alternate = this->prefs4_.collide == 1;
  r.rotate_s = (int64_t) this->prefs2_.rotate_minutes * 60;
  r.dwell_s = 10;
  return r;
}

std::vector<::espn::FavGame> GamedayComponent::fav_games_() const {
  std::vector<::espn::FavGame> out;
  for (const auto &e : this->fav_) {
    ::espn::FavGame g;
    g.valid = e.game.valid;
    g.state = e.game.state;
    g.kickoff_epoch = e.game.kickoff_epoch;
    g.final_epoch = e.final_epoch;
    out.push_back(g);
  }
  return out;
}

// Runs the pick and puts the chosen entry's card on the board when it differs
// from what is up. Returns true on a switch.
bool GamedayComponent::fav_choose_(int64_t now_epoch) {
  ::espn::FavChoice c = ::espn::pick_favorite(this->fav_games_(), now_epoch, this->fav_rules_(), this->fav_shown_,
                                              this->fav_shown_since_, this->fav_pinned_);
  bool was_locked = this->fav_locked_;
  this->fav_locked_ = c.locked;
  if (this->fav_pinned_ >= 0 && !c.locked && c.index != this->fav_pinned_)
    this->fav_pinned_ = -1;  // the playlist moved past the pinned card
  if (c.index == this->fav_shown_) {
    if (was_locked != c.locked)
      this->fav_poll_ms_ = 0;
    return false;
  }
  this->fav_shown_ = c.index;
  this->fav_shown_since_ = now_epoch;
  this->fav_poll_ms_ = 0;
  this->prev_ = GameSnapshot{};
  this->game_ = c.index >= 0 ? this->fav_[c.index].game : GameSnapshot{};
  this->post_since_ms_ = 0;
  this->misses_ = 0;
  if (c.index >= 0)
    ESP_LOGI(TAG, "Favorites: showing %s%s", this->fav_[c.index].team->abbr, c.locked ? " (locked)" : "");
  this->emit_({});
  return true;
}

// Main loop: refresh a stale entry, else poll the locked game when due, else tick.
void GamedayComponent::start_fav_job_(uint32_t now) {
  Job &j = this->job_;
  int64_t tnow = (int64_t) this->time_->timestamp_now();
  ::espn::FavRules rules = this->fav_rules_();
  for (auto &e : this->fav_) {
    if (e.game.valid && e.game.state == GameState::POST && e.final_epoch != 0 && !e.stale &&
        tnow - e.final_epoch >= rules.release_s)
      e.stale = true;  // released: the team endpoint now points at the next game
  }
  int refresh = -1;
  for (size_t i = 0; i < this->fav_.size(); i++) {
    const FavEntry &e = this->fav_[i];
    bool missing = !e.sched.valid || e.stale;
    uint32_t age = now - e.fetched_ms;
    if ((missing && (e.fetched_ms == 0 || age >= RETRY_INTERVAL)) || (!missing && age >= SCHEDULE_INTERVAL)) {
      refresh = (int) i;
      break;
    }
  }
  this->fav_choose_(tnow);
  if (refresh < 0 && this->fav_locked_ && this->fav_shown_ >= 0) {
    uint32_t due = this->interval_for_phase_();
    if (this->fav_poll_ms_ == 0 || now - this->fav_poll_ms_ >= due)
      refresh = -2;  // poll the shown game
  }
  if (refresh == -1) {
    this->schedule_next_(FAV_TICK);
    return;
  }
  int index = refresh >= 0 ? refresh : this->fav_shown_;
  FavEntry &e = this->fav_[index];
  j = Job{};
  j.generation = this->generation_;
  j.team = e.team;
  j.our_id = e.team->espn_id;
  j.fav_index = index;
  j.need_schedule = refresh >= 0;
  j.schedule = e.sched;
  if (refresh >= 0)
    e.fetched_ms = now == 0 ? 1 : now;
  this->job_done_ = false;
  this->busy_ = true;
  BaseType_t ok = xTaskCreate(&GamedayComponent::worker_, "gameday_fetch", 16384, this, 1, nullptr);
  if (ok != pdPASS) {
    ESP_LOGW(TAG, "Could not start the fetch task");
    this->busy_ = false;
    this->schedule_next_(RETRY_INTERVAL);
  }
}

void GamedayComponent::apply_fav_job_(uint32_t now) {
  Job &j = this->job_;
  if (j.fav_index < 0 || j.fav_index >= (int) this->fav_.size() || this->fav_[j.fav_index].team != j.team) {
    this->schedule_next_(0);  // the list changed while the worker ran
    return;
  }
  FavEntry &e = this->fav_[j.fav_index];
  int64_t tnow = (int64_t) this->time_->timestamp_now();
  bool shown = j.fav_index == this->fav_shown_;
  if (shown)
    this->fav_poll_ms_ = now == 0 ? 1 : now;
  if (j.need_schedule) {
    if (!j.schedule_ok) {
      this->misses_++;
      this->schedule_next_(0);
      return;
    }
    e.sched = j.schedule;
    e.sched.league = (uint8_t) e.team->league;
    e.stale = false;
    if (j.no_event) {
      ESP_LOGI(TAG, "No upcoming game for %s", e.team->abbr);
      e.game = GameSnapshot{};
      e.final_epoch = 0;
      if (shown)
        this->game_ = GameSnapshot{};
      if (!this->fav_choose_(tnow))
        this->rebuild_state_(&this->last_fields_);
      this->schedule_next_(0);
      return;
    }
  }
  if (!j.game_ok) {
    this->misses_++;
    this->schedule_next_(0);
    return;
  }
  GameSnapshot old = e.game;
  e.game = j.game;
  if (!old.valid || old.event_id != e.game.event_id)
    e.final_epoch = 0;
  if (e.game.state == GameState::POST && old.valid && old.event_id == e.game.event_id && old.state != GameState::POST)
    e.final_epoch = tnow;
  this->misses_ = 0;
  bool switched = this->fav_choose_(tnow);
  if (!switched && shown) {
    // Same game on the board: render the new poll with its splash.
    this->prev_ = this->game_;
    this->game_ = e.game;
    ::espn::Splash splash = ::espn::decide_splash(this->prev_, this->game_, this->opponent_splashes(), false);
    this->emit_(splash);
  } else if (!switched) {
    this->rebuild_state_(&this->last_fields_);  // the page's list of favorites' games
  }
  this->schedule_next_(0);
}

uint32_t GamedayComponent::our_id_() const {
  if (this->job_.fav_index >= 0)
    return this->job_.our_id;
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
  if (this->demo_active_) {
    this->demo_tick_();
    if (!this->busy_)
      return;
  }
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
    if (name == option) {
      this->apply_team_(t);
      return;
    }
  }
  ESP_LOGW(TAG, "Unknown team option '%s'", option.c_str());
}

void GamedayComponent::select_team_id(League league, uint32_t id) {
  for (size_t i = 0; i < ::espn::kTeamCount; i++) {
    const auto &t = ::espn::kTeams[i];
    if (t.league == league && t.espn_id == id) {
      this->apply_team_(t);
      return;
    }
  }
  ESP_LOGW(TAG, "Unknown team %s %u", league == League::NFL ? "nfl" : "ncaa", (unsigned) id);
}

void GamedayComponent::apply_team_(const ::espn::Team &t) {
  if ((uint8_t) t.league == this->prefs_.league && t.espn_id == this->prefs_.team_id)
    return;
  this->prefs_.league = (uint8_t) t.league;
  this->prefs_.team_id = t.espn_id;
  this->save_prefs_();
  ESP_LOGI(TAG, "Team changed to %s", t.name);
  bool first_pick = !this->flag_(FLAG_SETUP);
  if (first_pick) {
    this->prefs_.flags |= FLAG_SETUP;
    this->save_prefs_();
  }
  if (this->team_select_ != nullptr)
    this->team_select_->publish_state(this->team_option_());
  if (first_pick)
    for (auto &cb : this->action_callbacks_)
      cb("team_picked");  // the setup screen listens for this
  this->reset_game_();
  this->generation_++;  // a fetch already in flight belongs to the old team
  // Show the new team right away; the game data follows in a second or two.
  GameSnapshot g;
  g.valid = true;
  g.state = GameState::PRE;
  g.team_abbr = t.abbr;
  g.team_id = t.espn_id;
  g.team_logo = ::espn::team_logo_url(t.league, t.espn_id, t.abbr);
  g.short_detail = "Loading";
  this->game_ = g;
  this->emit_({});
  this->schedule_next_(0);
}

void GamedayComponent::select_timezone(const std::string &option) {
  for (size_t i = 0; i < ::espn::kTimezoneCount; i++) {
    if (option != ::espn::kTimezones[i].name)
      continue;
    this->prefs_.tz_index = (uint8_t) i;
    this->save_prefs_();
    this->apply_timezone_();
    if (this->timezone_select_ != nullptr)
      this->timezone_select_->publish_state(::espn::kTimezones[i].name);
    // Re-render so a PRE ticker picks up the new kickoff time.
    if (this->game_.valid)
      this->emit_({});
    else
      this->rebuild_state_(&this->last_fields_);
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
  else
    this->rebuild_state_(&this->last_fields_);
}

void GamedayComponent::save_prefs_() { this->pref_.save(&this->prefs_); }

void GamedayComponent::apply_timezone_() {
#ifdef GAMEDAY_TZ_PARSED
  // ESPHome 2026.9+: no POSIX parser on the device, hand the clock the
  // pre-parsed rules (scripts/build_timezones.py keeps the table in step).
  const ::espn::TzParsed &p = ::espn::kTimezonesParsed[this->prefs_.tz_index];
  auto rule = [](const ::espn::TzRule &r) {
    time::DSTRule d{};
    d.time_seconds = r.time_seconds;
    d.day = r.day;
    d.type = (time::DSTRuleType) r.type;
    d.month = r.month;
    d.week = r.week;
    d.day_of_week = r.day_of_week;
    return d;
  };
  time::ParsedTimezone tz{};
  tz.std_offset_seconds = p.std_offset_seconds;
  tz.dst_offset_seconds = p.dst_offset_seconds;
  tz.dst_start = rule(p.dst_start);
  tz.dst_end = rule(p.dst_end);
  time::set_global_tz(tz);
#else
  if (this->time_ != nullptr)
    this->time_->set_timezone(::espn::kTimezones[this->prefs_.tz_index].posix);
#endif
}

void GamedayComponent::reset_game_() {
  this->rebuild_favorites_(true);
  this->live_away_id_ = 0;
  this->live_started_ms_ = 0;
  this->live_none_ = false;
  this->schedule_ = Schedule{};
  this->schedule_fetched_ms_ = 0;
  this->upcoming_.clear();
  this->upcoming_due_ = false;
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

bool GamedayComponent::fetch_upcoming_(const ::espn::Team *team, std::vector<::espn::Upcoming> &out) {
  std::string url = ::espn::schedule_url(team->league, team->espn_id);
  ESP_LOGD(TAG, "Fetching upcoming games: %s", url.c_str());
  auto container = this->open_(url);
  if (container == nullptr)
    return false;
  ContainerReader reader(container);
  // Four: the game on the board plus the three after it.
  bool ok = ::espn::parse_upcoming(reader, team->espn_id, 4, out);
  container->end();
  ESP_LOGI(TAG, "Upcoming: %u game(s) (%u bytes)", (unsigned) out.size(), (unsigned) reader.total());
  return ok;
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
  League league = this->live_mode_() || this->job_.fav_index >= 0 ? (League) schedule.league : team->league;
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
  if (this->favorites_mode_()) {
    this->start_fav_job_(now);
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
  // The season schedule is ~200KB; it is fetched on a cycle of its own right
  // after the board has its game, never in front of it.
  if (this->upcoming_due_ && !j.need_schedule && this->schedule_.valid) {
    j.need_upcoming = true;
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
  if (j.need_upcoming) {
    j.upcoming_ok = this->fetch_upcoming_(j.team, j.upcoming);
    return;
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
  if (j.fav_index >= 0) {
    this->apply_fav_job_(now);
    return;
  }
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
  if (j.need_upcoming) {
    this->upcoming_due_ = false;
    if (j.upcoming_ok)
      this->upcoming_ = j.upcoming;
    this->rebuild_state_(&this->last_fields_);
    this->schedule_next_(this->interval_for_phase_());
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
    this->upcoming_due_ = true;  // refreshed on the next cycle, once the board is drawn
    if (j.no_event) {
      ESP_LOGI(TAG, "No upcoming game for this team");
      this->game_ = GameSnapshot{};
      this->misses_ = 0;
      this->emit_({});
      this->schedule_next_(0);
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
  this->schedule_next_(this->upcoming_due_ ? 0 : this->interval_for_phase_());
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
    f.clock_text = ::espn::clock_text(g);
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
  f.kickoff = kickoff;
  f.status_text = ::espn::status_text(g, opts, kickoff);
  if (this->live_mode_() && (!g.valid || g.state == GameState::NOT_FOUND))
    f.status_text = this->live_none_ ? "No live games right now" : "Looking for a live game";
  if (this->prefs2_.mode == (uint8_t) Mode::FAVORITES && this->fav_.empty())
    f.status_text = "Favorites: add teams on the page | " + f.status_text;
  if (this->misses_ >= 3)
    f.status_text += " *";
  // Until a team has been picked once, the ticker says where the setup page is.
  if (!this->flag_(FLAG_SETUP) && network::is_connected()) {
    std::string ip;
    for (auto &a : network::get_ip_addresses()) {
      if (a.is_set() && a.is_ip4()) {
        char buf[48];
        a.str_to(buf);
        ip = buf;
        break;
      }
    }
    if (!ip.empty())
      f.status_text = "Setup: open " + this->hostname() + ".local or " + ip + " on your phone | " + f.status_text;
  }

  if (!f.splash_text.empty())
    ESP_LOGI(TAG, "Splash: %s", f.splash_text.c_str());
  this->last_fields_ = f;
  this->rebuild_state_(&f);
  for (auto &cb : this->callbacks_)
    cb(f);
}

// ---- demo ---------------------------------------------------------------------

struct DemoStep {
  GameState state;
  int us, them;
  const char *clock;
  int period;
  int possession;  // 1 us, 2 them
  const char *down;
  bool red_zone;
  const char *play;
  uint32_t hold_ms;
};

// A believable drive-by-drive script. Splashes come from decide_splash on the
// score deltas, so the panel celebrates exactly as it would in a real game.
static const DemoStep DEMO[] = {
    {GameState::PRE, 0, 0, "", 0, 0, "", false, "", 3500},
    {GameState::IN, 0, 0, "15:00", 1, 1, "1st & 10", false, "Kickoff", 4000},
    {GameState::IN, 0, 0, "9:41", 1, 1, "3rd & 4", true, "Pass short left for 8 yards", 3500},
    {GameState::IN, 7, 0, "9:35", 1, 2, "", false, "Pass deep right for 21 yards, TOUCHDOWN", 5000},
    {GameState::IN, 7, 3, "2:10", 1, 1, "1st & 10", false, "38 yard field goal is GOOD", 4500},
    {GameState::IN, 7, 3, "11:22", 2, 2, "2nd & 7", false, "Rush up the middle for 3 yards", 3500},
    {GameState::IN, 7, 10, "6:48", 2, 1, "", false, "Pass deep middle for 44 yards, TOUCHDOWN", 5000},
    {GameState::IN, 14, 10, "1:03", 2, 2, "", false, "Rush right end for 12 yards, TOUCHDOWN", 5000},
    {GameState::IN, 14, 10, "0:00", 2, 0, "", false, "End of the 2nd quarter", 3500},
    {GameState::IN, 21, 10, "8:15", 4, 2, "", false, "Interception returned 31 yards, TOUCHDOWN", 5000},
    {GameState::IN, 24, 10, "2:00", 4, 2, "4th & 9", true, "27 yard field goal is GOOD", 4000},
    {GameState::POST, 24, 10, "", 4, 0, "", false, "", 6000},
};
static const size_t DEMO_STEPS = sizeof(DEMO) / sizeof(DEMO[0]);

void GamedayComponent::start_demo() {
  if (this->demo_active_)
    return;
  ESP_LOGI(TAG, "Demo: starting");
  this->demo_saved_game_ = this->game_;
  this->demo_saved_prev_ = this->prev_;
  this->generation_++;  // any fetch in flight belongs to before the demo
  this->demo_active_ = true;
  this->demo_step_ = 0;
  this->demo_next_ms_ = millis();
}

void GamedayComponent::demo_tick_() {
  uint32_t now = millis();
  if ((int32_t) (now - this->demo_next_ms_) < 0)
    return;
  if (this->demo_step_ >= DEMO_STEPS) {
    ESP_LOGI(TAG, "Demo: done, back to live data");
    this->demo_active_ = false;
    this->game_ = this->demo_saved_game_;
    this->prev_ = this->demo_saved_prev_;
    this->emit_({});
    this->schedule_next_(0);
    return;
  }
  const DemoStep &d = DEMO[this->demo_step_];
  // Keep the real sides (logos already decoded, colors, records); fall back
  // to a stock matchup on a panel that has nothing loaded yet.
  GameSnapshot g = this->demo_saved_game_;
  if (!g.valid || g.team_abbr.empty()) {
    const ::espn::Team *t = this->current_team_();
    g = GameSnapshot{};
    g.team_abbr = t ? t->abbr : "DAL";
    g.team_id = t ? t->espn_id : 6;
    g.team_logo = t ? ::espn::team_logo_url(t->league, t->espn_id, t->abbr) : "";
    g.opp_abbr = "PHI";
    g.opp_id = 21;
    g.opp_logo = ::espn::team_logo_url(League::NFL, 21, "PHI");
    g.team_color = "041E42";
    g.opp_color = "004C54";
    g.team_record = "1-0";
    g.opp_record = "1-0";
  }
  g.valid = true;
  g.state = d.state;
  g.completed = d.state == GameState::POST;
  g.team_score = d.us;
  g.opp_score = d.them;
  g.display_clock = d.clock;
  g.period = d.period;
  g.possession = d.possession;
  g.short_down_distance = d.down;
  g.down_distance = d.down;
  g.is_red_zone = d.red_zone;
  g.last_play = d.play;
  g.team_winner = d.state == GameState::POST && d.us > d.them;
  g.team_timeouts = d.period <= 2 ? 3 : 2;
  g.opp_timeouts = d.period <= 2 ? 3 : 1;
  if (d.state == GameState::PRE) {
    g.short_detail = "Tonight";
    g.kickoff_epoch = this->time_ != nullptr ? (int64_t) this->time_->timestamp_now() + 3600 : 0;
  } else if (d.state == GameState::POST) {
    g.short_detail = "Final";
  } else {
    g.short_detail = std::string(d.clock) + " - " + (d.period == 1 ? "1st" : d.period == 2 ? "2nd" : d.period == 3 ? "3rd" : "4th");
  }
  this->prev_ = this->game_;
  this->game_ = g;
  ::espn::Splash splash = this->demo_step_ == 0 ? ::espn::Splash{} : ::espn::decide_splash(this->prev_, g, true, false);
  this->emit_(splash);
  this->demo_next_ms_ = now + d.hold_ms;
  this->demo_step_++;
}

// ---- device page routes -----------------------------------------------------

static const char *const LEAGUE_KEY[] = {"nfl", "ncaa"};

static void put_team(JsonObject o, uint8_t league, uint32_t id) {
  for (size_t i = 0; i < ::espn::kTeamCount; i++) {
    const auto &t = ::espn::kTeams[i];
    if ((uint8_t) t.league == league && t.espn_id == id) {
      o["l"] = LEAGUE_KEY[league == (uint8_t) League::NFL ? 0 : 1];
      o["id"] = id;
      o["abbr"] = t.abbr;
      o["name"] = t.name;
      return;
    }
  }
}

// Serialises everything the page shows into state_json_. Main loop only; the
// HTTP task copies the finished string under the mutex.
void GamedayComponent::rebuild_state_(const UpdateFields *f) {
  JsonDocument doc;
  const GameSnapshot &g = this->game_;
  doc["name"] = this->hostname();
  doc["version"] = App.get_comment();
  doc["setup"] = this->flag_(FLAG_SETUP);
  doc["panels"] = this->panels_ != nullptr ? this->panels_->cols() : 0;
  JsonObject team = doc["team"].to<JsonObject>();
  put_team(team, this->prefs_.league, this->prefs_.team_id);
  doc["mode"] = this->prefs2_.mode;
  doc["rotate"] = this->prefs2_.rotate_minutes;
  doc["lockon"] = this->prefs4_.lockon_minutes;
  doc["release"] = this->prefs4_.release_seconds;
  doc["collide"] = this->prefs4_.collide;
  JsonArray favs = doc["favs"].to<JsonArray>();
  for (uint8_t i = 0; i < 4; i++) {
    JsonObject o = favs.add<JsonObject>();
    if (this->prefs3_.fav_id[i] != 0)
      put_team(o, this->prefs3_.fav_league[i], this->prefs3_.fav_id[i]);
  }
  doc["tz"] = this->prefs_.tz_index;
  doc["tz_name"] = ::espn::kTimezones[this->prefs_.tz_index].name;
  doc["tz_auto"] = this->tz_auto();
  doc["down"] = this->ticker_down_distance();
  doc["play"] = this->ticker_last_play();
  doc["odds"] = this->ticker_odds();
  doc["opp"] = this->opponent_splashes();
  doc["bootaddr"] = this->show_boot_address();
  doc["misses"] = this->misses_;

  JsonObject game = doc["game"].to<JsonObject>();
  game["s"] = g.valid ? ::espn::state_name(g.state) : "NOT_FOUND";
  uint8_t league = this->live_mode_() && this->schedule_.valid ? this->schedule_.league : this->prefs_.league;
  if (this->favorites_mode_() && this->fav_shown_ >= 0)
    league = (uint8_t) this->fav_[this->fav_shown_].team->league;
  game["l"] = LEAGUE_KEY[league == (uint8_t) League::NFL ? 0 : 1];
  game["ta"] = g.team_abbr;
  game["ti"] = g.team_id;
  game["ts"] = g.team_score;
  game["oa"] = g.opp_abbr;
  game["oi"] = g.opp_id;
  game["os"] = g.opp_score;
  game["tr"] = g.team_record;
  game["or"] = g.opp_record;
  game["p"] = g.possession;
  game["tt"] = g.team_timeouts;
  game["ot"] = g.opp_timeouts;
  game["rz"] = g.is_red_zone;
  game["tv"] = g.tv;
  game["venue"] = g.venue;
  game["odds"] = g.odds;
  game["ou"] = g.over_under;
  game["detail"] = g.short_detail;
  game["kick"] = g.kickoff_epoch;
  game["lp"] = g.last_play;
  game["dd"] = g.down_distance;
  if (f != nullptr) {
    game["c"] = f->clock_text;
    game["d"] = f->down_distance;
    game["k"] = f->kickoff;
    char color[8];
    snprintf(color, sizeof(color), "%06X", (unsigned) f->team_color);
    game["tc"] = color;
    snprintf(color, sizeof(color), "%06X", (unsigned) f->opponent_color);
    game["oc"] = color;
    doc["status"] = f->status_text;
    doc["splash"] = f->splash_text;
    snprintf(color, sizeof(color), "%06X", (unsigned) f->splash_color);
    doc["splash_color"] = color;
  }
  game["id"] = g.event_id;
  JsonArray next = doc["next"].to<JsonArray>();
  if (this->favorites_mode_()) {
    // One row per favorite: its next game, or the one in progress.
    doc["shown"] = this->fav_shown_ >= 0 ? (int) this->fav_[this->fav_shown_].slot : 0;
    doc["locked"] = this->fav_locked_;
    for (const auto &e : this->fav_) {
      JsonObject o = next.add<JsonObject>();
      o["slot"] = e.slot;
      JsonObject t = o["t"].to<JsonObject>();
      put_team(t, (uint8_t) e.team->league, e.team->espn_id);
      if (!e.game.valid)
        continue;
      o["id"] = e.game.event_id;
      o["s"] = ::espn::state_name(e.game.state);
      o["kick"] = e.game.kickoff_epoch;
      o["oi"] = e.game.opp_id;
      o["oa"] = e.game.opp_abbr;
      o["ts"] = e.game.team_score;
      o["os"] = e.game.opp_score;
      o["tv"] = e.game.tv;
      o["detail"] = e.game.short_detail;
    }
  }
  for (const auto &u : this->upcoming_) {
    JsonObject o = next.add<JsonObject>();
    o["id"] = u.event_id;
    o["kick"] = u.kickoff_epoch;
    o["oi"] = u.opp_id;
    o["oa"] = u.opp_abbr;
    o["on"] = u.opp_name;
    o["home"] = u.home;
    o["neutral"] = u.neutral;
    o["tv"] = u.tv;
  }
  std::string out;
  serializeJson(doc, out);
  std::lock_guard<std::mutex> lock(this->state_mutex_);
  this->state_json_ = std::move(out);
}

std::string GamedayComponent::current_state_json_() {
  std::lock_guard<std::mutex> lock(this->state_mutex_);
  return this->state_json_;
}

bool GamedayComponent::canHandle(AsyncWebServerRequest *request) const {
  char buf[AsyncWebServerRequest::URL_BUF_SIZE];
  return request->url_to(buf).starts_with("/gameday/");
}

void GamedayComponent::handleRequest(AsyncWebServerRequest *request) {
  char buf[AsyncWebServerRequest::URL_BUF_SIZE];
  StringRef url = request->url_to(buf);
  if (url == StringRef("/gameday/state") && request->method() == HTTP_GET) {
    std::string body = this->current_state_json_();
    request->send(200, "application/json", body.c_str());
    return;
  }
  if (url == StringRef("/gameday/set") && request->method() == HTTP_POST) {
    this->handle_set_(request);
    return;
  }
  if (url == StringRef("/gameday/action") && request->method() == HTTP_POST) {
    std::string name = request->arg("do");
    if (name.empty()) {
      request->send(400, "application/json", "{\"error\":\"do is required\"}");
      return;
    }
    this->defer([this, name]() {
      if (name == "refresh")
        this->refresh_now();
      else if (name == "demo")
        this->start_demo();
      for (auto &cb : this->action_callbacks_)
        cb(name);
    });
    request->send(200, "application/json", "{\"ok\":true}");
    return;
  }
  request->send(404, "application/json", "{\"error\":\"unknown route\"}");
}

static const char *const SET_KEYS[] = {"team",     "mode",   "rotate",  "fav1",    "fav2", "fav3",
                                       "fav4",     "tz",     "tzauto",  "down",    "play", "odds",
                                       "opp",      "panels", "bootaddr", "lockon", "release", "collide"};

void GamedayComponent::handle_set_(AsyncWebServerRequest *request) {
  std::vector<std::pair<std::string, std::string>> kv;
  for (const char *key : SET_KEYS)
    if (request->hasParam(key))
      kv.emplace_back(key, request->arg(key));
  if (kv.empty()) {
    request->send(400, "application/json", "{\"error\":\"nothing to set\"}");
    return;
  }
  this->defer([this, kv]() { this->apply_set_(kv); });
  request->send(200, "application/json", "{\"ok\":true}");
}

// "nfl:6" -> league + id. False for "none"/"" (id 0) or garbage.
static bool parse_team_ref(const std::string &v, uint8_t &league, uint32_t &id) {
  league = 0;
  id = 0;
  size_t colon = v.find(':');
  if (colon == std::string::npos)
    return false;
  std::string lg = v.substr(0, colon);
  id = (uint32_t) strtoul(v.c_str() + colon + 1, nullptr, 10);
  if (lg == "nfl")
    league = (uint8_t) League::NFL;
  else if (lg == "ncaa")
    league = (uint8_t) League::NCAA;
  else
    return false;
  return id != 0;
}

void GamedayComponent::apply_set_(const std::vector<std::pair<std::string, std::string>> &kv) {
  bool dirty = false;
  for (const auto &p : kv) {
    const std::string &k = p.first;
    const std::string &v = p.second;
    uint8_t league;
    uint32_t id;
    if (k == "team") {
      if (parse_team_ref(v, league, id))
        this->select_team_id((League) league, id);
    } else if (k == "mode") {
      int m = atoi(v.c_str());
      if (m >= 0 && m <= (int) Mode::FAVORITES)
        this->select_mode(MODE_OPTIONS[m]);
    } else if (k == "rotate") {
      this->set_rotate_minutes(atoi(v.c_str()));
    } else if (k == "lockon") {
      this->set_lockon_minutes(atoi(v.c_str()));
    } else if (k == "release") {
      this->set_release_seconds(atoi(v.c_str()));
    } else if (k == "collide") {
      this->set_collision_alternate(v == "1");
    } else if (k.size() == 4 && k.compare(0, 3, "fav") == 0) {
      uint8_t slot = (uint8_t) (k[3] - '0');
      if (!parse_team_ref(v, league, id) && v != "none" && !v.empty()) {
        ESP_LOGW(TAG, "Bad favorite '%s'", v.c_str());
        continue;
      }
      this->set_favorite_(slot, league, id);
    } else if (k == "tz") {
      int i = atoi(v.c_str());
      if (i >= 0 && i < (int) ::espn::kTimezoneCount)
        this->select_timezone(::espn::kTimezones[i].name);
    } else if (k == "tzauto") {
      this->set_tz_auto(v == "1");
      dirty = true;
    } else if (k == "down") {
      this->set_ticker_down_distance(v == "1");
    } else if (k == "play") {
      this->set_ticker_last_play(v == "1");
    } else if (k == "odds") {
      this->set_ticker_odds(v == "1");
    } else if (k == "opp") {
      this->set_opponent_splashes(v == "1");
    } else if (k == "bootaddr") {
      this->set_show_boot_address(v == "1");
    } else if (k == "panels") {
      if (this->panels_ != nullptr)
        this->panels_->set_cols((uint8_t) atoi(v.c_str()));
    }
  }
  if (dirty)
    this->rebuild_state_(&this->last_fields_);
}

}  // namespace gameday
}  // namespace esphome
