#include "gameday.h"

#include "esphome/components/json/json_util.h"
#include "esphome/components/network/util.h"
#include "esphome/core/application.h"
#include "esphome/core/hal.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"

#include <cmath>
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
// A final never changes: one poll for the card, then only the rescan.
static const uint32_t FINAL_CARD_INTERVAL = 30 * MINUTE;
// The league's next game this week is looked up again after this long.
static const uint32_t NEXT_REFRESH = 60 * MINUTE;
static const uint32_t DAY = 24 * 60 * MINUTE;
static const uint32_t WEATHER_INTERVAL = 30 * MINUTE;
// Standings and weather are extras: a failure waits a while before retrying.
static const uint32_t IDLE_RETRY = 5 * MINUTE;
// How long the loop waits on a worker task before writing it off. A job chains
// at most two requests (schedule then game, or the NFL then the NCAA
// scoreboard). http_request in gameday-common.yaml allows each one 20s, with a
// 30s watchdog_timeout as the hard ceiling, so two requests can legitimately
// spend 60s on the network alone. Add the TLS handshakes and the streamed
// parse of a Saturday NCAA scoreboard and the old 60s deadline was under the
// worst honest case, not over it. 2 x watchdog_timeout for the network plus
// 60s for handshakes and parsing leaves only a genuinely wedged worker here.
static const uint32_t WORKER_DEADLINE = 120 * 1000;

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
  this->pref5_ = global_preferences->make_preference<Prefs5>(fnv1_hash("gameday_prefs5_v1"));
  bool loaded = this->pref5_.load(&this->prefs5_);
  uint8_t off = this->prefs5_.idle_off_min;
  if (!loaded || this->prefs5_.fallback > 1 || this->prefs5_.idle_screens > idle::ALL_SCREENS ||
      this->prefs5_.idle_rotate_s < 10 || this->prefs5_.idle_rotate_s > 600 || this->prefs5_.wx_set > 1 ||
      (off != 0 && off != 15 && off != 30 && off != 60 && off != 120)) {
    this->prefs5_ = Prefs5{};
    this->prefs5_.idle_screens = idle::DEFAULT_SCREENS;
    this->prefs5_.idle_rotate_s = 60;
  }
  this->prefs5_.wx_grid[sizeof(this->prefs5_.wx_grid) - 1] = '\0';
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
    // Somebody asked for a different mode, so they are looking at the panel.
    this->fire_action_("user_pick");
    return;
  }
  ESP_LOGW(TAG, "Unknown mode '%s'", option.c_str());
}

void GamedayComponent::set_rotate_minutes(int minutes) {
  if (minutes < 1)
    minutes = 1;
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

void GamedayComponent::set_fallback_my_team(bool my_team) {
  if ((my_team ? 1 : 0) == this->prefs5_.fallback)
    return;
  this->prefs5_.fallback = my_team ? 1 : 0;
  this->pref5_.save(&this->prefs5_);
  ESP_LOGI(TAG, "Live fallback: %s", my_team ? "my team" : "next game");
  if (this->live_mode_()) {
    // Re-decide from a fresh scan rather than leave the old choice up.
    this->reset_game_();
    this->generation_++;
    this->emit_({});
    this->schedule_next_(0);
    return;
  }
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
  this->start_worker_();
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
    ::espn::adopt_league(e.sched, e.team->league);
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
  this->mark_good_poll_();
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
  // In the my_team fallback the panel is showing the saved team's own game,
  // so the snapshot is oriented around that team, exactly as in My team mode.
  if (this->live_mode_() && !this->team_fallback_)
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
  this->idle_tick_();
  if (this->demo_active_) {
    this->demo_tick_();
    if (!this->busy_)
      return;
  }
  if (this->busy_) {
    if (!this->job_done_) {
      // A worker that died without setting job_done_ would block the loop
      // forever and Refresh Now could never clear it. Give up eventually, but
      // not before a slow-but-honest job could have finished: abandoning a
      // live worker is what makes the accepted run_job_/job_ write race
      // reachable, and that must stay out of reach on a merely slow network.
      if (this->busy_since_ms_ != 0 && (millis() - this->busy_since_ms_) >= WORKER_DEADLINE) {
        ESP_LOGW(TAG, "Fetch task did not finish in %us, giving up on it", (unsigned) (WORKER_DEADLINE / 1000));
        this->busy_ = false;
        this->busy_since_ms_ = 0;
        this->worker_seq_++;  // a late finisher must not signal job_done_
        // Wait a full retry before starting another worker rather than
        // stacking a second one on top of a task that may still be alive.
        this->schedule_next_(RETRY_INTERVAL);
      }
      return;
    }
    this->busy_ = false;
    this->busy_since_ms_ = 0;
    this->job_done_ = false;
    this->apply_job_();
    return;
  }
  if ((int32_t) (millis() - this->next_fetch_ms_) < 0) {
    // The mode has nothing due: an idle screen's data may be.
    uint32_t now = millis();
    if (this->idle_ && now - this->idle_job_check_ms_ >= 5000 && network::is_connected()) {
      this->idle_job_check_ms_ = now;
      this->start_idle_job_(now);
    }
    return;
  }
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

void GamedayComponent::fire_action_(const std::string &name) {
  for (auto &cb : this->action_callbacks_)
    cb(name);
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
    this->fire_action_("team_picked");  // the setup screen listens for this
  // The select, the page and the app all land here, and only on a real
  // change, so this is the one place that knows a person picked a team.
  this->fire_action_("user_pick");
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
  // Read the team endpoint again, do not just re-poll the game already on
  // the board: the owner presses this when the board looks wrong, and a
  // moved, postponed or finished game only shows up in that read.
  this->force_schedule_ = true;
  // Live modes: scan now, and look the next game up again if it is needed.
  this->last_scan_ms_ = 0;
  for (auto &n : this->next_)
    n.fetched_ms = 0;
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
  this->last_scan_ms_ = 0;
  this->card_polled_ms_ = 0;
  this->live_show_ = LiveShow::NONE;
  this->team_fallback_ = false;
  this->finals_.clear();
  this->later_ = ::espn::LiveGame{};
  this->final_idx_ = 0;
  this->final_since_ms_ = 0;
  this->next_pending_ = 0;
  this->schedule_ = Schedule{};
  this->schedule_fetched_ms_ = 0;
  this->force_schedule_ = false;
  this->upcoming_.clear();
  this->upcoming_due_ = false;
  this->game_ = GameSnapshot{};
  this->prev_ = GameSnapshot{};
  this->post_since_ms_ = 0;
  this->misses_ = 0;
}

std::shared_ptr<http_request::HttpContainer> GamedayComponent::open_(const std::string &url, int *status,
                                                                    const char *accept) {
  // NWS also requires a User-Agent; the same one serves both services.
  std::vector<http_request::Header> headers = {
      {"User-Agent", USER_AGENT},
      {"Accept", accept},
  };
  auto container = this->http_->get(url, headers);
  if (container == nullptr) {
    ESP_LOGW(TAG, "Request failed: %s", url.c_str());
    return nullptr;
  }
  if (status != nullptr)
    *status = container->status_code;
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

bool GamedayComponent::fetch_upcoming_(const ::espn::Team *team, ::espn::TeamSchedule &out) {
  std::string url = ::espn::schedule_url(team->league, team->espn_id);
  ESP_LOGD(TAG, "Fetching upcoming games: %s", url.c_str());
  auto container = this->open_(url);
  if (container == nullptr)
    return false;
  ContainerReader reader(container);
  // Four: the game on the board plus the three after it.
  bool ok = ::espn::parse_team_schedule(reader, team->espn_id, 4, out);
  container->end();
  ESP_LOGI(TAG, "Upcoming: %u game(s), record %s (%u bytes)", (unsigned) out.upcoming.size(), out.record.c_str(),
           (unsigned) reader.total());
  return ok;
}

bool GamedayComponent::fetch_scan_(League league, Job &j) {
  std::string url = ::espn::scan_url(league, (int64_t) this->time_->timestamp_now());
  ESP_LOGD(TAG, "Scanning: %s", url.c_str());
  auto container = this->open_(url);
  if (container == nullptr)
    return false;
  ContainerReader reader(container);
  ::espn::ScanResult scan;
  bool ok = ::espn::parse_scan(reader, scan);
  container->end();
  for (auto &g : scan.live) {
    g.league = (uint8_t) league;
    j.live.push_back(g);
  }
  for (auto &g : scan.finals) {
    g.league = (uint8_t) league;
    j.finals.push_back(g);
  }
  if (scan.later.state == GameState::PRE) {
    scan.later.league = (uint8_t) league;
    if (j.later.state != GameState::PRE || scan.later.kickoff_epoch < j.later.kickoff_epoch)
      j.later = scan.later;
  }
  ESP_LOGI(TAG, "Scan: %u live, %u final, next %s (%u events, %u bytes)", (unsigned) scan.live.size(),
           (unsigned) scan.finals.size(), scan.later.state == GameState::PRE ? scan.later.event_id.c_str() : "-",
           (unsigned) scan.events, (unsigned) reader.total());
  return ok;
}

// The league's next game this week: the week view, then the week after it
// when every game this week has started.
bool GamedayComponent::fetch_next_(League league, ::espn::LiveGame &out) {
  out = ::espn::LiveGame{};
  int week = 0;
  for (int attempt = 0; attempt < 2; attempt++) {
    std::string url = ::espn::week_url(league, week);
    ESP_LOGD(TAG, "Next game: %s", url.c_str());
    auto container = this->open_(url);
    if (container == nullptr)
      return false;
    ContainerReader reader(container);
    ::espn::ScanResult scan;
    bool ok = ::espn::parse_scan(reader, scan);
    container->end();
    if (!ok)
      return false;
    ESP_LOGI(TAG, "Next game: week %d, %s (%u bytes)", scan.week,
             scan.later.state == GameState::PRE ? scan.later.event_id.c_str() : "none", (unsigned) reader.total());
    if (scan.later.state == GameState::PRE) {
      out = scan.later;
      out.league = (uint8_t) league;
      return true;
    }
    if (scan.week <= 0)
      return true;
    week = scan.week + 1;
  }
  return true;
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
  uint32_t ms;
  switch (this->game_.state) {
    case GameState::IN:
      ms = IN_INTERVAL;
      break;
    case GameState::POST:
      ms = POST_INTERVAL;
      break;
    case GameState::PRE: {
      int64_t now = (int64_t) this->time_->timestamp_now();
      int64_t until = this->game_.kickoff_epoch - now;
      ms = until <= PRE_NEAR_SECONDS ? PRE_NEAR_INTERVAL : PRE_FAR_INTERVAL;
      break;
    }
    default:
      ms = SCHEDULE_INTERVAL;
      break;
  }
  // The fallback card is only borrowing the board: a 15 minute PRE cycle, or
  // the 6 hours an out of season team gets, must never outlast the rescan.
  if (this->team_fallback_) {
    uint32_t since = millis() - this->last_scan_ms_;
    uint32_t left = since >= NO_LIVE_RESCAN ? 0 : NO_LIVE_RESCAN - since;
    if (ms > left)
      ms = left;
  }
  return ms;
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
    uint32_t since_scan = now - this->last_scan_ms_;
    bool rescan_due = this->last_scan_ms_ == 0 || since_scan >= NO_LIVE_RESCAN;
    for (uint8_t l = 0; l < 2; l++) {
      if ((this->next_pending_ & (1 << l)) == 0)
        continue;
      j.need_next = true;
      j.next_league = l;
      this->start_worker_();
      return;
    }
    bool scan;
    if (this->team_fallback_) {
      scan = rescan_due;
    } else if (this->live_show_ == LiveShow::LIVE) {
      bool rotate = this->live_started_ms_ != 0 &&
                    (now - this->live_started_ms_) >= (uint32_t) this->prefs2_.rotate_minutes * MINUTE;
      bool ended = this->game_.valid && this->game_.state != GameState::IN;
      scan = !this->schedule_.valid || rotate || ended;
    } else {
      scan = rescan_due || this->live_show_ == LiveShow::NONE;
    }
    if (scan) {
      j.need_scan = true;
      // Stamped here, not when the scan lands, so a failed scan does not
      // fire another one on the retry a minute later.
      this->last_scan_ms_ = now == 0 ? 1 : now;
      j.schedule = this->schedule_;
      this->start_worker_();
      return;
    }
    bool card = this->live_show_ == LiveShow::LATER_TODAY || this->live_show_ == LiveShow::FINAL_TODAY ||
                this->live_show_ == LiveShow::NEXT_GAME;
    if (card && this->schedule_.valid) {
      // A card is re-polled on its own clock, never past the next rescan.
      uint32_t due;
      if (this->game_.state == GameState::POST)
        due = FINAL_CARD_INTERVAL;
      else if (this->game_.state == GameState::IN)
        due = IN_INTERVAL;
      else
        due = (this->game_.kickoff_epoch - (int64_t) this->time_->timestamp_now()) <= PRE_NEAR_SECONDS
                  ? PRE_NEAR_INTERVAL
                  : PRE_FAR_INTERVAL;
      uint32_t age = now - this->card_polled_ms_;
      if (this->game_.valid && this->card_polled_ms_ != 0 && age < due) {
        uint32_t wait = std::min(due - age, NO_LIVE_RESCAN - since_scan);
        this->schedule_next_(wait);
        return;
      }
    }
    if ((card || this->live_show_ == LiveShow::LIVE) && this->schedule_.valid) {
      j.schedule = this->schedule_;
      this->start_worker_();
      return;
    }
    if (!this->team_fallback_) {
      // Nothing to poll (idle, or waiting on a lookup): wait for the rescan.
      this->schedule_next_(NO_LIVE_RESCAN - since_scan);
      return;
    }
    // In the my_team fallback with the rescan not due: fall through to the
    // My team path below, so the card is fetched and polled by that code and
    // not by a copy of it.
  }
  j.need_schedule = ::espn::schedule_due(this->schedule_.valid, this->force_schedule_, now,
                                         this->schedule_fetched_ms_, SCHEDULE_INTERVAL);
  // The season schedule is ~200KB; it is fetched on a cycle of its own right
  // after the board has its game, never in front of it.
  if (this->upcoming_due_ && !j.need_schedule && this->schedule_.valid) {
    j.need_upcoming = true;
    j.schedule = this->schedule_;
    this->start_worker_();
    return;
  }
  if (!j.need_schedule && this->schedule_.valid && this->schedule_.event_id.empty()) {
    // No game on the schedule (off season): there is no scoreboard to poll,
    // only the team endpoint to re-read when it is due.
    uint32_t age = now - this->schedule_fetched_ms_;
    uint32_t wait = age >= SCHEDULE_INTERVAL ? 0 : SCHEDULE_INTERVAL - age;
    if (this->team_fallback_) {
      uint32_t since = now - this->last_scan_ms_;
      wait = std::min(wait, since >= NO_LIVE_RESCAN ? 0 : NO_LIVE_RESCAN - since);
    }
    this->schedule_next_(wait);
    return;
  }
  if (this->post_since_ms_ != 0 && (now - this->post_since_ms_) >= POST_LINGER) {
    // Game is over and lingered: look for the next one. Re-reading the team
    // endpoint is not enough on its own, because it can keep pointing at a
    // game that finished hours ago. The season schedule lists only games that
    // have not started, so prefer it when it has one.
    int64_t tnow = (int64_t) this->time_->timestamp_now();
    const ::espn::Upcoming *next = nullptr;
    for (const auto &u : this->upcoming_) {
      if (u.kickoff_epoch > tnow && u.event_id != this->game_.event_id) {
        next = &u;
        break;
      }
    }
    this->prev_ = GameSnapshot{};
    this->game_ = GameSnapshot{};
    this->post_since_ms_ = 0;
    if (next != nullptr) {
      ESP_LOGI(TAG, "Game is over: taking event %s from the season schedule", next->event_id.c_str());
      // group, colors and the record are team properties from the team
      // endpoint and stay valid; only the event moves.
      this->schedule_.event_id = next->event_id;
      this->schedule_.kickoff_epoch = next->kickoff_epoch;
      this->schedule_fetched_ms_ = now == 0 ? 1 : now;
      this->upcoming_due_ = true;  // the list just lost an entry: refresh it
    } else {
      j.need_schedule = true;  // no list yet: a fresh boot or the end of a season
    }
  }
  j.schedule = this->schedule_;
  this->start_worker_();
}

// Hands this->job_ to a fresh worker task.
void GamedayComponent::start_worker_() {
  this->job_done_ = false;
  this->busy_ = true;
  this->busy_since_ms_ = millis() == 0 ? 1 : millis();
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
  uint32_t mine = self->worker_seq_;
  self->run_job_();
  // A worker the loop gave up on must not hand back a result: the Job it
  // was writing into belongs to a later cycle by now.
  if (self->worker_seq_ == mine)
    self->job_done_ = true;
  vTaskDelete(nullptr);
}

// Worker task: network and parsing only, no display or entity access.
void GamedayComponent::run_job_() {
  Job &j = this->job_;
  if (j.need_team_sched) {
    j.team_sched_ok = this->fetch_upcoming_(j.team, j.team_sched);
    return;
  }
  if (j.need_standings) {
    j.standings_ok = this->fetch_standings_(j.team->league, j.standings_group, j.standings);
    return;
  }
  if (j.need_points) {
    this->fetch_points_(j.wx_lat, j.wx_lon, j.wx_grid, j.points_status);
    return;
  }
  if (j.need_weather) {
    j.weather_ok = this->fetch_weather_(j.wx_grid, j.weather);
    return;
  }
  if (j.need_scan) {
    j.scan_ok = true;
    if (this->league_wanted_(League::NFL))
      j.scan_ok = this->fetch_scan_(League::NFL, j) && j.scan_ok;
    if (this->league_wanted_(League::NCAA))
      j.scan_ok = this->fetch_scan_(League::NCAA, j) && j.scan_ok;
    return;  // the main loop picks a game, then the next cycle polls it
  }
  if (j.need_next) {
    j.next_ok = this->fetch_next_((League) j.next_league, j.next);
    return;
  }
  if (j.need_upcoming) {
    j.upcoming_ok = this->fetch_upcoming_(j.team, j.team_sched);
    return;
  }
  if (j.need_schedule) {
    Schedule s;
    j.schedule_ok = this->fetch_schedule_(j.team, s);
    if (!j.schedule_ok)
      return;
    j.schedule = s;
    ::espn::adopt_league(j.schedule, j.team->league);
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
  if (j.need_team_sched || j.need_standings || j.need_points || j.need_weather) {
    this->apply_idle_job_(now);
    return;
  }
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
    int64_t tnow = (int64_t) this->time_->timestamp_now();
    this->finals_ = ::espn::finals_today(j.finals, tnow, ESPTime::timezone_offset());
    this->later_ = j.later;
    this->decide_live_(now, j.live);
    return;
  }
  if (j.need_next) {
    this->next_pending_ &= (uint8_t) ~(1 << j.next_league);
    if (!j.next_ok) {
      // Unknown stays unknown: the next rescan asks again.
      this->misses_++;
      this->schedule_next_(RETRY_INTERVAL);
      return;
    }
    NextCache &c = this->next_[j.next_league & 1];
    c.fetched_ms = now == 0 ? 1 : now;
    c.found = j.next.state == GameState::PRE;
    c.game = j.next;
    if (this->next_pending_ == 0)
      this->decide_live_(now, {});
    else
      this->schedule_next_(0);
    return;
  }
  if (j.need_upcoming) {
    this->upcoming_due_ = false;
    if (j.upcoming_ok) {
      this->upcoming_ = j.team_sched.upcoming;
      // The same download feeds the idle screens.
      this->team_sched_ = j.team_sched;
      this->team_sched_team_ = j.team;
      this->team_sched_ms_ = now == 0 ? 1 : now;
    }
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
    this->force_schedule_ = false;
    this->upcoming_due_ = true;  // refreshed on the next cycle, once the board is drawn
    if (j.no_event) {
      ESP_LOGI(TAG, "No upcoming game for this team");
      this->game_ = GameSnapshot{};
      // Same as the no-live-games case: the schedule fetch succeeded, so the
      // failure run is over, but no GameSnapshot was applied and the board
      // has no score whose age last_good_ms_ could describe.
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
  this->mark_good_poll_();
  ::espn::Splash splash =
      ::espn::decide_splash(this->prev_, this->game_, this->opponent_splashes(),
                            this->live_mode_() && !this->team_fallback_);
  if (this->game_.state == GameState::POST) {
    if (this->post_since_ms_ == 0)
      this->post_since_ms_ = now == 0 ? 1 : now;
  } else {
    this->post_since_ms_ = 0;
  }
  this->emit_(splash);
  if (this->live_mode_() && !this->team_fallback_ && this->live_show_ != LiveShow::LIVE) {
    // A later, final or next-game card: start_job_ times its next poll.
    this->card_polled_ms_ = now == 0 ? 1 : now;
    this->schedule_next_(0);
    return;
  }
  this->schedule_next_(this->upcoming_due_ ? 0 : this->interval_for_phase_());
}

bool GamedayComponent::league_wanted_(League league) const {
  uint8_t mode = this->prefs2_.mode;
  if (league == League::NFL)
    return mode == (uint8_t) Mode::LIVE_NFL || mode == (uint8_t) Mode::LIVE_ANY;
  return mode == (uint8_t) Mode::LIVE_NCAA || mode == (uint8_t) Mode::LIVE_ANY;
}

// Across the mode's leagues: unknown if any lookup is missing or stale.
::espn::NextState GamedayComponent::next_state_(int64_t now_epoch) const {
  bool found = false;
  for (uint8_t l = 0; l < 2; l++) {
    if (!this->league_wanted_((League) l))
      continue;
    const NextCache &c = this->next_[l];
    bool stale = c.fetched_ms == 0 || (millis() - c.fetched_ms) >= NEXT_REFRESH ||
                 (c.found && c.game.kickoff_epoch <= now_epoch);
    if (stale)
      return ::espn::NextState::UNKNOWN;
    found = found || c.found;
  }
  return found ? ::espn::NextState::FOUND : ::espn::NextState::NONE;
}

// Live modes: what goes on the board after a scan (or a next-game lookup).
// The order lives in espn::live_precedence so the host tests cover it.
void GamedayComponent::decide_live_(uint32_t now, const std::vector<::espn::LiveGame> &live) {
  int64_t tnow = (int64_t) this->time_->timestamp_now();
  ::espn::LiveInputs in;
  in.any_live = !live.empty();
  in.my_team_fallback = this->fallback_my_team() && this->current_team_() != nullptr;
  in.later_today = this->later_.state == GameState::PRE;
  in.finals_today = this->finals_.size();
  // Only looked up when nothing above it applies: that is one request saved
  // on every scan of a game day.
  bool need_next = !in.any_live && !in.my_team_fallback && !in.later_today && in.finals_today == 0;
  in.next = need_next ? this->next_state_(tnow) : ::espn::NextState::NONE;
  LiveShow show = ::espn::live_precedence(in);
  LiveShow was = this->live_show_;
  switch (show) {
    case LiveShow::LIVE: {
      // Prefer a different game than the one we just showed.
      std::vector<size_t> pick;
      for (size_t i = 0; i < live.size(); i++)
        if (live[i].event_id != this->schedule_.event_id)
          pick.push_back(i);
      if (pick.empty())
        pick.push_back(0);
      const ::espn::LiveGame &g = live[pick[random_uint32() % pick.size()]];
      ESP_LOGI(TAG, "Following %s @ %s", g.away_abbr.c_str(), g.home_abbr.c_str());
      this->live_show_ = LiveShow::LIVE;
      this->team_fallback_ = false;
      this->schedule_ = Schedule{};
      this->schedule_.valid = true;
      this->schedule_.event_id = g.event_id;
      this->schedule_.group = g.group;
      this->schedule_.league = g.league;
      this->schedule_.kickoff_epoch = tnow;
      this->schedule_fetched_ms_ = now;
      this->live_away_id_ = g.away_id;
      this->live_started_ms_ = now == 0 ? 1 : now;
      this->game_ = GameSnapshot{};
      this->prev_ = GameSnapshot{};
      this->schedule_next_(0);  // poll the chosen game right away
      return;
    }
    case LiveShow::MY_TEAM: {
      const ::espn::Team *saved = this->current_team_();
      bool already = this->team_fallback_;
      this->live_show_ = LiveShow::MY_TEAM;
      this->team_fallback_ = true;
      // No freshness marking here on purpose. What is on the board in the
      // fallback is the saved team's card, and only a poll of that card can
      // say how old it is; the card's own poll marks it good when it lands.
      if (already) {
        // The card is already on the board: leave it there, and let the My
        // team path keep polling it rather than reloading it every scan.
        ESP_LOGI(TAG, "Still no live games: staying on %s", saved->abbr);
        this->rebuild_state_(&this->last_fields_);
        this->schedule_next_(0);
        return;
      }
      ESP_LOGI(TAG, "No live games right now: showing %s", saved->abbr);
      this->live_away_id_ = 0;  // the card is oriented around the saved team
      // Clearing the schedule is what forces the re-read; zeroing the
      // timestamp alone would not, because the test is on schedule_.valid.
      this->schedule_ = Schedule{};
      this->schedule_fetched_ms_ = 0;
      this->game_ = GameSnapshot{};
      this->prev_ = GameSnapshot{};
      this->post_since_ms_ = 0;
      this->emit_({});
      this->schedule_next_(0);  // fetch the card now
      return;
    }
    case LiveShow::LATER_TODAY:
      this->live_show_ = show;
      this->show_live_card_(this->later_, now);
      return;
    case LiveShow::FINAL_TODAY: {
      // One final at a time, moving on every "Switch every" minutes.
      if (was != LiveShow::FINAL_TODAY) {
        this->final_idx_ = 0;
        this->final_since_ms_ = now;
      } else if (now - this->final_since_ms_ >= (uint32_t) this->prefs2_.rotate_minutes * MINUTE) {
        this->final_idx_++;
        this->final_since_ms_ = now;
      }
      this->live_show_ = show;
      this->show_live_card_(this->finals_[this->final_idx_ % this->finals_.size()], now);
      return;
    }
    case LiveShow::NEXT_GAME: {
      const ::espn::LiveGame *best = nullptr;
      for (uint8_t l = 0; l < 2; l++) {
        const NextCache &c = this->next_[l];
        if (this->league_wanted_((League) l) && c.found &&
            (best == nullptr || c.game.kickoff_epoch < best->kickoff_epoch))
          best = &c.game;
      }
      if (best == nullptr)
        break;
      this->live_show_ = show;
      this->show_live_card_(*best, now);
      return;
    }
    case LiveShow::FETCH_NEXT:
      for (uint8_t l = 0; l < 2; l++) {
        const NextCache &c = this->next_[l];
        bool stale = c.fetched_ms == 0 || (now - c.fetched_ms) >= NEXT_REFRESH ||
                     (c.found && c.game.kickoff_epoch <= tnow);
        if (this->league_wanted_((League) l) && stale)
          this->next_pending_ |= (uint8_t) (1 << l);
      }
      this->live_show_ = show;
      this->schedule_next_(0);
      return;
    default:
      break;
  }
  // Nothing at all this week.
  if (was != LiveShow::IDLE)
    ESP_LOGI(TAG, "Nothing live, today or this week");
  this->live_show_ = LiveShow::IDLE;
  this->team_fallback_ = false;
  this->schedule_ = Schedule{};
  this->game_ = GameSnapshot{};
  this->prev_ = GameSnapshot{};
  // The scan came back clean, so there is no run of failures to report.
  // last_good_ms_ stays where it was: no game data was applied.
  this->misses_ = 0;
  this->emit_({});
  this->schedule_next_(NO_LIVE_RESCAN);
}

// Puts a scanned game (later today, a final, next this week) on the board.
// The same game again keeps its card; a new one is polled right away.
void GamedayComponent::show_live_card_(const ::espn::LiveGame &g, uint32_t now) {
  bool same = this->schedule_.valid && this->schedule_.event_id == g.event_id && this->game_.valid;
  this->team_fallback_ = false;
  this->schedule_ = Schedule{};
  this->schedule_.valid = true;
  this->schedule_.event_id = g.event_id;
  this->schedule_.group = g.group;
  this->schedule_.league = g.league;
  // The follow-up poll reads the scoreboard for the game's own day.
  this->schedule_.kickoff_epoch = g.kickoff_epoch;
  this->schedule_fetched_ms_ = now;
  this->live_away_id_ = g.away_id;
  if (same) {
    this->emit_({});  // the ticker may have changed with the show
    this->schedule_next_(0);
    return;
  }
  ESP_LOGI(TAG, "Showing %s @ %s", g.away_abbr.c_str(), g.home_abbr.c_str());
  this->game_ = GameSnapshot{};
  this->prev_ = GameSnapshot{};
  this->post_since_ms_ = 0;
  this->card_polled_ms_ = 0;
  this->schedule_next_(0);
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
  if (this->live_mode_()) {
    // "college", "NFL", or nothing at all in Live any, with its own space.
    std::string league_word;
    if (this->prefs2_.mode == (uint8_t) Mode::LIVE_NCAA)
      league_word = "college ";
    else if (this->prefs2_.mode == (uint8_t) Mode::LIVE_NFL)
      league_word = "NFL ";
    // The my_team fallback shows the saved team's own ticker, unprefixed.
    if (!this->team_fallback_)
      f.status_text = ::espn::live_ticker(this->live_show_, league_word, g, opts, kickoff, f.status_text);
  }
  if (this->prefs2_.mode == (uint8_t) Mode::FAVORITES && this->fav_.empty())
    f.status_text = "Favorites: add teams on the page | " + f.status_text;
  if (this->misses_ >= 3) {
    if (!this->ever_polled_good_()) {
      // Nothing has ever come back, so there is no age to quote. Saying "no
      // update for 1 min" here was wrong however long the panel had been
      // trying, on a screen someone is reading because something is broken.
      f.status_text += " | no update yet";
    } else {
      uint32_t mins = this->stale_seconds_() / 60;
      if (mins < 1)
        mins = 1;
      f.status_text += " | no update for " + std::to_string(mins) + " min";
    }
  }
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

// ---- idle screens -------------------------------------------------------------

void GamedayComponent::set_idle_screens(uint8_t mask) {
  mask &= idle::ALL_SCREENS;
  if (mask == this->prefs5_.idle_screens)
    return;
  this->prefs5_.idle_screens = mask;
  this->save_prefs5_();
  if (this->idle_)
    this->render_idle_(false);
  this->rebuild_state_(&this->last_fields_);
}

void GamedayComponent::set_idle_rotate_seconds(int seconds) {
  seconds = seconds < 10 ? 10 : seconds > 600 ? 600 : seconds;
  if (seconds == this->prefs5_.idle_rotate_s)
    return;
  this->prefs5_.idle_rotate_s = (uint16_t) seconds;
  this->save_prefs5_();
  this->rebuild_state_(&this->last_fields_);
}

void GamedayComponent::set_idle_off_minutes(int minutes) {
  if (minutes != 0 && minutes != 15 && minutes != 30 && minutes != 60 && minutes != 120)
    return;
  if (minutes == this->prefs5_.idle_off_min)
    return;
  this->prefs5_.idle_off_min = (uint8_t) minutes;
  this->save_prefs5_();
  this->idle_woke_ms_ = millis();  // a new setting counts from now
  this->rebuild_state_(&this->last_fields_);
}

void GamedayComponent::set_weather_location(bool set, float lat, float lon) {
  if (set && (!(lat >= -90.0f && lat <= 90.0f) || !(lon >= -180.0f && lon <= 180.0f)))
    return;
  if (set && this->prefs5_.wx_set && fabsf(lat - this->prefs5_.wx_lat) < 0.00005f &&
      fabsf(lon - this->prefs5_.wx_lon) < 0.00005f)
    return;
  if (!set && !this->prefs5_.wx_set)
    return;
  this->prefs5_.wx_set = set ? 1 : 0;
  this->prefs5_.wx_lat = set ? lat : 0.0f;
  this->prefs5_.wx_lon = set ? lon : 0.0f;
  this->prefs5_.wx_grid[0] = '\0';  // looked up again for the new place
  this->save_prefs5_();
  this->weather_ = ::nws::Weather{};
  this->weather_ms_ = 0;
  this->weather_try_ms_ = 0;
  ESP_LOGI(TAG, "Weather location: %s", set ? "set" : "cleared");
  if (this->idle_)
    this->render_idle_(false);
  this->rebuild_state_(&this->last_fields_);
}

void GamedayComponent::on_power_on() {
  this->idle_dark_ = false;
  this->idle_woke_ms_ = millis();
}

// The mode has nothing to put on the scoreboard.
bool GamedayComponent::idle_wanted_() const {
  if (this->demo_active_)
    return false;
  // The saved team's schedule came back with no game in it (off season).
  bool team_empty = this->schedule_.valid && this->schedule_.event_id.empty() && !this->game_.valid;
  if (this->live_mode_())
    return this->team_fallback_ ? team_empty : this->live_show_ == LiveShow::IDLE;
  if (this->favorites_mode_()) {
    for (const auto &e : this->fav_)
      if (!e.sched.valid || e.game.valid)
        return false;  // still loading, or something to show
    return true;
  }
  return team_empty;
}

const ::espn::Upcoming *GamedayComponent::team_next_game_(int64_t now_epoch) const {
  if (!this->team_sched_.valid || this->team_sched_team_ != this->current_team_())
    return nullptr;
  for (const auto &u : this->team_sched_.upcoming)
    if (u.kickoff_epoch > now_epoch)
      return &u;
  return nullptr;
}

// Screens that have something true to show right now.
uint8_t GamedayComponent::idle_available_() const {
  uint8_t have = idle::bit(idle::CLOCK);
  int64_t tnow = (int64_t) this->time_->timestamp_now();
  bool ours = this->team_sched_.valid && this->team_sched_team_ == this->current_team_();
  if (this->team_next_game_(tnow) != nullptr)
    have |= idle::bit(idle::COUNTDOWN);
  if (this->standings_.valid && ours)
    have |= idle::bit(idle::STANDINGS);
  if (ours && !this->team_sched_.record.empty())
    have |= idle::bit(idle::RECORD);
  // A forecast older than three hours says nothing about now.
  if (this->weather_.valid && this->prefs5_.wx_set && millis() - this->weather_ms_ < 3 * 60 * MINUTE)
    have |= idle::bit(idle::WEATHER);
  return have;
}

IdleFields GamedayComponent::idle_fields_(int screen) {
  IdleFields f;
  f.screen = idle::screen_name(screen);
  const ::espn::Team *t = this->current_team_();
  if (t == nullptr)
    return f;
  bool wide = this->panels_ != nullptr && this->panels_->cols() >= 2;
  struct tm now = this->time_->now().to_c_tm();
  int64_t tnow = (int64_t) this->time_->timestamp_now();
  const ::espn::Upcoming *next = this->team_next_game_(tnow);
  std::string color = this->team_sched_.color.empty() ? this->schedule_.team_color : this->team_sched_.color;
  f.color = ::espn::parse_color(color);
  std::string us = t->abbr;
  std::string vs, kick;
  if (next != nullptr) {
    vs = us + (next->home || next->neutral ? " vs " : " at ") + next->opp_abbr;
    struct tm k = ESPTime::from_epoch_local((time_t) next->kickoff_epoch).to_c_tm();
    kick = ::espn::kickoff_label(k, now);
  }
  std::string our_logo = ::espn::team_logo_url(t->league, t->espn_id, t->abbr);
  std::string their_logo = next != nullptr ? ::espn::team_logo_url(t->league, next->opp_id, next->opp_abbr.c_str()) : "";
  switch (screen) {
    case idle::COUNTDOWN:
      if (next == nullptr)
        break;
      f.big = idle::countdown_text(next->kickoff_epoch - tnow, wide);
      f.line1 = vs;
      f.line2 = kick;
      f.line3 = next->tv;
      f.team_logo = our_logo;
      f.opp_logo = their_logo;
      break;
    case idle::STANDINGS: {
      f.line1 = this->standings_.title;
      const auto &rows = this->standings_.rows;
      int64_t shown_s = (int64_t) ((millis() - this->idle_screen_ms_) / 1000);
      size_t page = idle::standings_page(rows.size(), 4, shown_s, 20);
      for (size_t i = 0; i < 4 && page * 4 + i < rows.size(); i++) {
        const auto &r = rows[page * 4 + i];
        f.rows[i] = std::to_string(page * 4 + i + 1) + " " + r.abbr + " " + r.record;
        if (r.team_id == t->espn_id)
          f.highlight = (int) i;
      }
      break;
    }
    case idle::RECORD:
      f.big = us + " " + this->team_sched_.record;
      f.line1 = ::espn::result_line(this->team_sched_.last);
      if (next != nullptr)
        f.line2 = std::string("Next: ") + (next->home || next->neutral ? "vs " : "at ") + next->opp_abbr;
      f.line3 = this->team_sched_.standing;
      f.team_logo = our_logo;
      break;
    case idle::WEATHER: {
      char buf[24];
      snprintf(buf, sizeof(buf), "%d\xC2\xB0", this->weather_.temp);
      f.big = buf;
      f.line1 = this->weather_.condition;
      snprintf(buf, sizeof(buf), "H %d  L %d", this->weather_.high, this->weather_.low);
      f.line2 = buf;
      f.line3 = idle::clock_text(now) + (now.tm_hour < 12 ? " AM" : " PM");
      break;
    }
    default:
      f.screen = idle::screen_name(idle::CLOCK);
      f.big = idle::clock_text(now);
      if (next != nullptr) {
        f.line1 = vs;
        f.line2 = kick;
        f.team_logo = our_logo;
        f.opp_logo = their_logo;
      } else {
        f.line1 = idle::date_text(now);
        f.line2 = now.tm_hour < 12 ? "AM" : "PM";
      }
      break;
  }
  return f;
}

// Sends the current screen to the pages. show switches pages; otherwise it
// only goes out when the text changed (the minute, a countdown second).
void GamedayComponent::render_idle_(bool show) {
  if (!this->idle_ || this->idle_screen_ < 0)
    return;
  // A screen the owner turned off, or whose data went away (location
  // cleared, the game kicked off), gives way to the next one. With nothing
  // left to show, that is the clock.
  uint8_t ok = this->prefs5_.idle_screens & this->idle_available_();
  bool keep = ok != 0 ? (ok & (1u << this->idle_screen_)) != 0 : this->idle_screen_ == idle::CLOCK;
  if (!keep) {
    this->idle_screen_ = idle::next_screen(this->prefs5_.idle_screens, this->idle_available_(), this->idle_screen_);
    this->idle_screen_ms_ = millis();
    show = true;
    this->rebuild_state_(&this->last_fields_);
  }
  IdleFields f = this->idle_fields_(this->idle_screen_);
  f.show = show;
  std::string key = f.screen + "|" + f.big + "|" + f.line1 + "|" + f.line2 + "|" + f.line3 + "|" + f.rows[0] + f.rows[1] +
                    f.rows[2] + f.rows[3] + "|" + f.team_logo + "|" + f.opp_logo;
  if (!show && key == this->idle_key_)
    return;
  this->idle_key_ = key;
  for (auto &cb : this->idle_callbacks_)
    cb(f);
}

// Main loop, every pass: enter or leave idle, rotate, tick the text, and the
// off-after-idle timer. Runs while a fetch is in flight so the clock ticks.
void GamedayComponent::idle_tick_() {
  uint32_t now = millis();
  if (now - this->idle_tick_ms_ < 500)
    return;
  this->idle_tick_ms_ = now;
  bool time_ok = this->time_ != nullptr && this->time_->now().is_valid();
  bool want = time_ok && this->idle_wanted_();
  if (want != this->idle_) {
    this->idle_ = want;
    this->idle_key_.clear();
    if (want) {
      ESP_LOGI(TAG, "Nothing to show: idle screens");
      this->idle_since_ms_ = now;
      this->idle_woke_ms_ = now;
      this->idle_screen_ = idle::next_screen(this->prefs5_.idle_screens, this->idle_available_(), -1);
      this->idle_screen_ms_ = now;
      this->idle_job_check_ms_ = 0;
      this->render_idle_(true);
    } else {
      ESP_LOGI(TAG, "Back to the scoreboard");
      this->idle_screen_ = -1;
      IdleFields f;  // an empty screen hands the panel back to the scoreboard
      f.show = true;
      for (auto &cb : this->idle_callbacks_)
        cb(f);
      if (this->idle_dark_) {
        this->idle_dark_ = false;
        this->fire_action_("idle_wake");
      }
    }
    this->rebuild_state_(&this->last_fields_);
    return;
  }
  if (!this->idle_)
    return;
  if (now - this->idle_screen_ms_ >= (uint32_t) this->prefs5_.idle_rotate_s * 1000) {
    int next = idle::next_screen(this->prefs5_.idle_screens, this->idle_available_(), this->idle_screen_);
    this->idle_screen_ms_ = now;
    if (next != this->idle_screen_) {
      this->idle_screen_ = next;
      this->render_idle_(true);
      this->rebuild_state_(&this->last_fields_);
    }
  }
  this->render_idle_(false);
  uint32_t off_ms = (uint32_t) this->prefs5_.idle_off_min * MINUTE;
  if (off_ms != 0 && !this->idle_dark_ && now - this->idle_woke_ms_ >= off_ms) {
    ESP_LOGI(TAG, "Idle for %u min: panel off until there is something to show", (unsigned) this->prefs5_.idle_off_min);
    this->idle_dark_ = true;
    this->fire_action_("idle_off");
  }
}

// While idle, keeps the idle screens' data fresh: the saved team's season
// (countdown, record, standings group), the standings once a day, the
// weather every half hour. One request per job, never next to a game fetch.
bool GamedayComponent::start_idle_job_(uint32_t now) {
  if (!this->idle_ || this->busy_)
    return false;
  const ::espn::Team *t = this->current_team_();
  if (t == nullptr)
    return false;
  Job &j = this->job_;
  auto may_try = [now](uint32_t tried, uint32_t wait) { return tried == 0 || now - tried >= wait; };
  bool sched_stale = !this->team_sched_.valid || this->team_sched_team_ != t ||
                     now - this->team_sched_ms_ >= SCHEDULE_INTERVAL;
  if (sched_stale && may_try(this->team_sched_try_ms_, RETRY_INTERVAL)) {
    this->team_sched_try_ms_ = now == 0 ? 1 : now;
    j = Job{};
    j.generation = this->generation_;
    j.team = t;
    j.need_team_sched = true;
    this->start_worker_();
    return true;
  }
  uint8_t on = this->prefs5_.idle_screens;
  if ((on & idle::bit(idle::STANDINGS)) && this->team_sched_.valid && this->team_sched_team_ == t &&
      this->team_sched_.group != 0) {
    uint32_t key = (uint32_t) t->league * 100000u + this->team_sched_.group + 1;
    bool stale = !this->standings_.valid || this->standings_key_ != key || now - this->standings_ms_ >= DAY;
    if (stale && may_try(this->standings_try_ms_, IDLE_RETRY)) {
      this->standings_try_ms_ = now == 0 ? 1 : now;
      j = Job{};
      j.generation = this->generation_;
      j.team = t;
      j.need_standings = true;
      j.standings_group = this->team_sched_.group;
      this->start_worker_();
      return true;
    }
  }
  if ((on & idle::bit(idle::WEATHER)) && this->prefs5_.wx_set && strcmp(this->prefs5_.wx_grid, "-") != 0) {
    bool need_grid = this->prefs5_.wx_grid[0] == '\0';
    bool stale = !this->weather_.valid || now - this->weather_ms_ >= WEATHER_INTERVAL;
    if ((need_grid || stale) && may_try(this->weather_try_ms_, IDLE_RETRY)) {
      this->weather_try_ms_ = now == 0 ? 1 : now;
      j = Job{};
      j.generation = this->generation_;
      j.team = t;
      j.wx_lat = this->prefs5_.wx_lat;
      j.wx_lon = this->prefs5_.wx_lon;
      if (need_grid) {
        j.need_points = true;
      } else {
        j.need_weather = true;
        j.wx_grid = this->prefs5_.wx_grid;
      }
      this->start_worker_();
      return true;
    }
  }
  return false;
}

void GamedayComponent::apply_idle_job_(uint32_t now) {
  Job &j = this->job_;
  uint32_t stamp = now == 0 ? 1 : now;
  if (j.need_team_sched && j.team_sched_ok) {
    this->team_sched_ = j.team_sched;
    this->team_sched_team_ = j.team;
    this->team_sched_ms_ = stamp;
  } else if (j.need_standings && j.standings_ok) {
    this->standings_ = j.standings;
    this->standings_key_ = (uint32_t) j.team->league * 100000u + j.standings_group + 1;
    this->standings_ms_ = stamp;
  } else if (j.need_points) {
    // The owner may have moved the location while the lookup ran.
    bool same = this->prefs5_.wx_set && j.wx_lat == this->prefs5_.wx_lat && j.wx_lon == this->prefs5_.wx_lon;
    if (same && (!j.wx_grid.empty() || j.points_status == 404)) {
      // 404 is NWS for "not a US point": stop asking until the location changes.
      std::string grid = j.wx_grid.empty() ? "-" : j.wx_grid;
      strncpy(this->prefs5_.wx_grid, grid.c_str(), sizeof(this->prefs5_.wx_grid) - 1);
      this->prefs5_.wx_grid[sizeof(this->prefs5_.wx_grid) - 1] = '\0';
      this->save_prefs5_();
      this->weather_try_ms_ = 0;  // forecast right away
      this->rebuild_state_(&this->last_fields_);
    }
  } else if (j.need_weather && j.weather_ok && this->prefs5_.wx_set && j.wx_grid == this->prefs5_.wx_grid) {
    this->weather_ = j.weather;
    this->weather_ms_ = stamp;
  }
  this->render_idle_(false);
}

bool GamedayComponent::fetch_standings_(League league, uint32_t group, ::espn::Standings &out) {
  std::string url = ::espn::standings_url(league, group);
  ESP_LOGD(TAG, "Fetching standings: %s", url.c_str());
  auto container = this->open_(url);
  if (container == nullptr)
    return false;
  ContainerReader reader(container);
  // A conference can have 18 teams; more than 20 rows is not a standings page.
  bool ok = ::espn::parse_standings(reader, 20, out);
  container->end();
  ESP_LOGI(TAG, "Standings: %s, %u teams (%u bytes)", out.title.c_str(), (unsigned) out.rows.size(),
           (unsigned) reader.total());
  return ok;
}

bool GamedayComponent::fetch_points_(float lat, float lon, std::string &grid, int &status) {
  std::string url = ::nws::points_url(lat, lon);
  ESP_LOGD(TAG, "Weather grid: %s", url.c_str());
  auto container = this->open_(url, &status, "application/geo+json");
  if (container == nullptr)
    return false;
  ContainerReader reader(container);
  bool ok = ::nws::parse_points(reader, grid);
  container->end();
  ESP_LOGI(TAG, "Weather grid: %s (%u bytes)", ok ? grid.c_str() : "not parsed", (unsigned) reader.total());
  return ok;
}

bool GamedayComponent::fetch_weather_(const std::string &grid, ::nws::Weather &out) {
  std::string url = ::nws::hourly_url(grid);
  ESP_LOGD(TAG, "Fetching weather: %s", url.c_str());
  auto container = this->open_(url, nullptr, "application/geo+json");
  if (container == nullptr)
    return false;
  ContainerReader reader(container);
  bool ok = ::nws::parse_hourly(reader, out);
  container->end();
  ESP_LOGI(TAG, "Weather: %d%s %s, high %d low %d (%u bytes)", out.temp, out.unit.c_str(), out.condition.c_str(),
           out.high, out.low, (unsigned) reader.total());
  return ok;
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
  doc["stale_s"] = this->stale_seconds_();
  // true while the my_team fallback has the board (the app reads it).
  doc["fallback"] = this->team_fallback_;
  doc["fallback_mode"] = this->fallback_my_team() ? "my_team" : "next_game";
  doc["idle"] = this->idle_;
  doc["idle_screen"] = this->idle_ ? idle::screen_name(this->idle_screen_) : "";
  doc["idle_screens"] = this->prefs5_.idle_screens;
  doc["idle_rotate"] = this->prefs5_.idle_rotate_s;
  doc["idle_off"] = this->prefs5_.idle_off_min;
  if (this->prefs5_.wx_set) {
    doc["wx_lat"] = this->prefs5_.wx_lat;
    doc["wx_lon"] = this->prefs5_.wx_lon;
  } else {
    doc["wx_lat"] = nullptr;
    doc["wx_lon"] = nullptr;
  }
  // "-" when NWS has no forecast for the location (outside the US).
  doc["wx_grid"] = this->prefs5_.wx_grid;

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
                                       "opp",      "panels", "bootaddr", "lockon", "release", "collide",
                                       "fallback", "idle",     "idlerot", "idleoff", "wxlat", "wxlon"};

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
  bool wx_seen = false;
  std::string wx_lat, wx_lon;
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
    } else if (k == "idle") {
      this->set_idle_screens((uint8_t) atoi(v.c_str()));
    } else if (k == "idlerot") {
      this->set_idle_rotate_seconds(atoi(v.c_str()));
    } else if (k == "idleoff") {
      this->set_idle_off_minutes(atoi(v.c_str()));
    } else if (k == "wxlat") {
      wx_seen = true;
      wx_lat = v;
    } else if (k == "wxlon") {
      wx_seen = true;
      wx_lon = v;
    } else if (k == "fallback") {
      if (v == "my_team" || v == "1")
        this->set_fallback_my_team(true);
      else if (v == "next_game" || v == "0")
        this->set_fallback_my_team(false);
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
  // Latitude and longitude arrive together; "none" (or empty) for both clears.
  if (wx_seen) {
    auto blank = [](const std::string &v) { return v.empty() || v == "none"; };
    if (blank(wx_lat) && blank(wx_lon))
      this->set_weather_location(false, 0, 0);
    else if (!blank(wx_lat) && !blank(wx_lon))
      this->set_weather_location(true, strtof(wx_lat.c_str(), nullptr), strtof(wx_lon.c_str(), nullptr));
  }
  if (dirty)
    this->rebuild_state_(&this->last_fields_);
}

}  // namespace gameday
}  // namespace esphome
