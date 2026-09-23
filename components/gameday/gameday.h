#pragma once

#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "esphome/components/http_request/http_request.h"
#include "esphome/components/panel_layout/panel_layout.h"
#include "esphome/components/select/select.h"
#include "esphome/components/time/real_time_clock.h"
#include "esphome/components/web_server_base/web_server_base.h"
#include "esphome/core/automation.h"
#include "esphome/core/component.h"
#include "esphome/core/preferences.h"

#include "espn_parse.h"
#include "favorites.h"
#include "idle.h"
#include "weather.h"

namespace esphome {
namespace gameday {

using ::espn::GameSnapshot;
using ::espn::GameState;
using ::espn::League;
using ::espn::LiveShow;
using ::espn::Schedule;
using ::espn::Splash;
using ::espn::TickerOptions;

// Exactly the fields the Home Assistant blueprint used to push to the page.
struct UpdateFields {
  std::string game_state;
  std::string team_abbr;
  int team_score{0};
  std::string opponent_abbr;
  int opponent_score{0};
  std::string status_text;
  std::string team_logo;
  std::string opponent_logo;
  int possession{0};
  int team_timeouts{0};
  int opponent_timeouts{0};
  std::string team_record;
  std::string opponent_record;
  std::string splash_text;
  uint32_t splash_color{0};
  std::string last_play;
  std::string clock_text;  // "5:36 - 3rd" while the game is live, else empty
  std::string down_distance;  // "3rd & 10" while live, else empty
  bool is_red_zone{false};
  uint32_t team_color{0xFFFFFF};
  uint32_t opponent_color{0xFFFFFF};
  std::string kickoff;  // "Sun 3:25 PM" style label while PRE, else empty
};

// One idle screen's content, for the LVGL pages in firmware/pages/idle.yaml.
struct IdleFields {
  std::string screen;  // "clock", "countdown", "standings", "record", "weather"; "" = back to the scoreboard
  bool show{false};    // switch to this screen's page now
  std::string big;     // the large line: time, countdown, record, temperature
  std::string line1, line2, line3;
  std::string rows[4];  // standings, one page
  int highlight{-1};    // the row that is the saved team
  uint32_t color{0xFFFFFF};  // the saved team's color
  std::string team_logo, opp_logo;  // empty = hide that logo
};

enum class SelectType : uint8_t { TEAM, TIMEZONE, MODE, FAVORITE };

enum class Mode : uint8_t { MY_TEAM = 0, LIVE_NFL = 1, LIVE_NCAA = 2, LIVE_ANY = 3, FAVORITES = 4 };

class GamedayComponent;

class GamedaySelect : public select::Select, public Component {
 public:
  void set_type(SelectType type) { this->type_ = type; }
  void set_slot(uint8_t slot) { this->slot_ = slot; }
  void set_parent(GamedayComponent *parent) { this->parent_ = parent; }

 protected:
  void control(const std::string &value) override;
  SelectType type_{SelectType::TEAM};
  uint8_t slot_{0};
  GamedayComponent *parent_{nullptr};
};

// The component is also the web handler for /gameday/*: the device page reads
// one JSON document and posts settings there instead of driving entities.
//   GET  /gameday/state              full snapshot + settings
//   POST /gameday/set?key=value...   team=nfl:6 mode=0-4 rotate=2-30 fav1..fav4=nfl:6|none
//                                    tz=<index> tzauto=0/1 down/play/odds/opp=0/1 panels=1/2
//                                    lockon=5-120 (min) release=30-3600 (s) collide=0 stick|1 alternate
//                                    fallback=next_game|my_team (live modes with nothing live)
//                                    idle=<bitmask> idlerot=10-600 (s) idleoff=0|15|30|60|120 (min)
//                                    wxlat=<deg> wxlon=<deg> (both; both "none" clears)
//   POST /gameday/action?do=refresh|demo   (demo: a scripted game on the panel, ~40s)
// Requests arrive on the HTTP task; settings are applied on the main loop.
class GamedayComponent : public Component, public AsyncWebHandler {
 public:
  void set_http(http_request::HttpRequestComponent *http) { this->http_ = http; }
  void set_time(time::RealTimeClock *time) { this->time_ = time; }
  void set_web_server_base(web_server_base::WebServerBase *base) { this->base_ = base; }
  void set_panel_layout(panel_layout::PanelLayout *p) { this->panels_ = p; }
  void add_on_action_callback(std::function<void(std::string)> &&cb) {
    this->action_callbacks_.push_back(std::move(cb));
  }
  void set_team_select(select::Select *s) { this->team_select_ = s; }
  void set_timezone_select(select::Select *s) { this->timezone_select_ = s; }
  void set_mode_select(select::Select *s) { this->mode_select_ = s; }
  void set_favorite_select(uint8_t slot, select::Select *s) {
    if (slot >= 1 && slot <= 4)
      this->favorite_selects_[slot - 1] = s;
  }
  void add_on_update_callback(std::function<void(const UpdateFields &)> &&cb) {
    this->callbacks_.push_back(std::move(cb));
  }
  void add_on_idle_callback(std::function<void(const IdleFields &)> &&cb) {
    this->idle_callbacks_.push_back(std::move(cb));
  }

  // Called by the selects and by template entities in YAML.
  void select_team(const std::string &option);
  void select_team_id(League league, uint32_t id);
  void select_timezone(const std::string &option);
  void select_mode(const std::string &option);
  std::string hostname() const;  // the device name, with its MAC suffix
  // Favorites: four team slots for the remote's numbered buttons.
  void select_favorite(uint8_t slot, const std::string &option);
  void press_favorite(uint8_t slot);
  void set_rotate_minutes(int minutes);
  int rotate_minutes() const { return this->prefs2_.rotate_minutes; }
  // Favorite Teams mode timing (see favorites.h for the rules).
  void set_lockon_minutes(int minutes);
  void set_release_seconds(int seconds);
  void set_collision_alternate(bool alternate);
  // Live modes with nothing live: the league's next thing (false), or the
  // saved team's own card as in v1.4.0 (true).
  void set_fallback_my_team(bool my_team);
  bool fallback_my_team() const { return this->prefs5_.fallback == 1; }
  // Idle screens (idle.h): which ones rotate, how often, and the off timer.
  void set_idle_screens(uint8_t mask);
  void set_idle_rotate_seconds(int seconds);
  void set_idle_off_minutes(int minutes);
  void set_weather_location(bool set, float lat, float lon);
  bool idle() const { return this->idle_; }
  // Something true is on the screen: a board from a real fetch, or an idle
  // screen. The boot overlay holds until then.
  bool content_ready() const { return this->content_ready_; }
  // The Power switch turned on, by anyone: the off-after-idle timer restarts.
  void on_power_on();
  // The Power switch turned off. By hand, it cancels the idle wake: the
  // panel stays off until someone turns it on.
  void on_power_off();
  // Tells off-after-idle whether the panel is lit (the Power switch).
  void set_power_state(std::function<bool()> &&f) { this->power_state_ = std::move(f); }
  // Which page LVGL shows: "clock", "countdown", "standings", "record",
  // "weather", "scoreboard" or "other". Polled from YAML once a second.
  void set_active_page(const char *page);
  void refresh_now();
  // Plays a scripted game through the real splash and render path, for
  // showing the panel off without a live game. Real data resumes after.
  void start_demo();

  bool ticker_clock() const { return this->flag_(FLAG_CLOCK); }
  bool ticker_down_distance() const { return this->flag_(FLAG_DOWN); }
  bool ticker_last_play() const { return this->flag_(FLAG_PLAY); }
  bool ticker_odds() const { return this->flag_(FLAG_ODDS); }
  bool opponent_splashes() const { return this->flag_(FLAG_OPP); }
  bool setup_done() const { return this->flag_(FLAG_SETUP); }
  // Stored inverted so panels from before this setting keep showing it.
  bool show_boot_address() const { return !this->flag_(FLAG_NOBOOTADDR); }
  void set_show_boot_address(bool on) { this->set_flag_(FLAG_NOBOOTADDR, !on); }
  // Stored inverted (a "manual" bit) so panels flashed before this existed
  // come up with auto on without touching their saved preferences.
  bool tz_auto() const { return !this->flag_(FLAG_TZMANUAL); }
  void set_tz_auto(bool on) { this->set_flag_(FLAG_TZMANUAL, !on); }
  void set_ticker_clock(bool on) { this->set_flag_(FLAG_CLOCK, on); }
  void set_ticker_down_distance(bool on) { this->set_flag_(FLAG_DOWN, on); }
  void set_ticker_last_play(bool on) { this->set_flag_(FLAG_PLAY, on); }
  void set_ticker_odds(bool on) { this->set_flag_(FLAG_ODDS, on); }
  void set_opponent_splashes(bool on) { this->set_flag_(FLAG_OPP, on); }

  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::LATE; }

  // AsyncWebHandler (HTTP task)
  bool canHandle(AsyncWebServerRequest *request) const override;
  void handleRequest(AsyncWebServerRequest *request) override;

 protected:
  void apply_team_(const ::espn::Team &t);
  void fire_action_(const std::string &name);
  void set_favorite_(uint8_t slot, uint8_t league, uint32_t id);
  void handle_set_(AsyncWebServerRequest *request);
  void apply_set_(const std::vector<std::pair<std::string, std::string>> &kv);
  void rebuild_state_(const UpdateFields *f);  // main loop only
  std::string current_state_json_();
  std::mutex state_mutex_;
  std::string state_json_{"{}"};
  UpdateFields last_fields_;
  web_server_base::WebServerBase *base_{nullptr};
  panel_layout::PanelLayout *panels_{nullptr};
  std::vector<std::function<void(std::string)>> action_callbacks_;
  Splash last_splash_;  // for the page: the splash goes out once per emit
  static constexpr uint8_t FLAG_CLOCK = 1;
  static constexpr uint8_t FLAG_DOWN = 2;
  static constexpr uint8_t FLAG_PLAY = 4;
  static constexpr uint8_t FLAG_ODDS = 8;
  static constexpr uint8_t FLAG_OPP = 16;
  static constexpr uint8_t FLAG_TZMANUAL = 32;  // timezone was picked by hand; page must not override
  static constexpr uint8_t FLAG_SETUP = 64;     // a team has been picked at least once
  static constexpr uint8_t FLAG_NOBOOTADDR = 128;  // skip the address flash on a configured panel's boot
  static constexpr uint8_t FLAGS_DEFAULT = FLAG_DOWN | FLAG_PLAY | FLAG_ODDS | FLAG_OPP;

  struct Prefs {
    uint8_t league;
    uint32_t team_id;
    uint8_t tz_index;
    uint8_t flags;
  } __attribute__((packed));
  // Kept separate so adding fields never invalidates the original blob.
  struct Prefs2 {
    uint8_t mode;
    uint8_t rotate_minutes;
  } __attribute__((packed));
  struct Prefs3 {
    uint8_t fav_league[4];
    uint32_t fav_id[4];  // 0 = slot empty
  } __attribute__((packed));
  struct Prefs4 {
    uint8_t lockon_minutes;
    uint16_t release_seconds;
    uint8_t collide;  // 0 = stick with the higher slot, 1 = alternate on the rotate timer
  } __attribute__((packed));
  struct Prefs5 {
    uint8_t fallback;        // 0 = the league's next thing, 1 = the saved team (v1.4.0)
    uint8_t idle_screens;    // idle::bit() mask
    uint16_t idle_rotate_s;  // seconds per idle screen
    uint8_t idle_off_min;    // 0 = never, else minutes of idle before the panel goes dark
    uint8_t wx_set;          // 1 when a weather location has been entered
    float wx_lat;
    float wx_lon;
    char wx_grid[24];  // NWS office/cell for that location; "" = not looked up, "-" = outside the US
  } __attribute__((packed));
  // The team endpoint's answer for the saved team, so a boot can go straight
  // to the scoreboard instead of waiting on a 23 KB team read first.
  struct Prefs6 {
    uint8_t league;
    uint32_t team_id;
    char event_id[16];
    int64_t kickoff_epoch;
    uint32_t group;
    char color[8];
    char record[12];
    int64_t saved_epoch;
  } __attribute__((packed));
  ESPPreferenceObject pref6_;
  bool team_cache_tried_{false};
  bool upcoming_after_poll_{false};  // booted from the cache: season list after the first board
  bool load_team_cache_(const ::espn::Team *team);
  void save_team_cache_(const ::espn::Team *team);

  bool flag_(uint8_t f) const { return (this->prefs_.flags & f) != 0; }
  void set_flag_(uint8_t f, bool on);
  void save_prefs_();
  void publish_selects_();
  void apply_timezone_();
  const ::espn::Team *current_team_() const;
  std::string team_option_() const;

  // One fetch cycle runs on its own FreeRTOS task so the display loop never
  // waits on the network. The main loop fills a Job, the worker performs the
  // HTTP requests and parsing into it, and the main loop applies the result.
  struct Job {
    uint32_t generation{0};
    const ::espn::Team *team{nullptr};
    // live-game modes: scan the day's games first, then follow one
    bool need_scan{false};
    std::vector<::espn::LiveGame> live;
    std::vector<::espn::LiveGame> finals;
    ::espn::LiveGame later;
    bool scan_ok{false};
    // live-game modes: the league's next game this week
    bool need_next{false};
    uint8_t next_league{0};
    ::espn::LiveGame next;
    bool next_ok{false};
    // idle screens: the saved team's season, standings, weather
    bool need_team_sched{false};
    ::espn::TeamSchedule team_sched;
    bool team_sched_ok{false};
    bool need_standings{false};
    uint32_t standings_group{0};
    ::espn::Standings standings;
    bool standings_ok{false};
    bool need_points{false};
    float wx_lat{0}, wx_lon{0};
    std::string wx_grid;
    int points_status{0};
    bool need_weather{false};
    ::nws::Weather weather;
    bool weather_ok{false};
    bool need_schedule{false};
    Schedule schedule;
    bool schedule_ok{false};
    // the "Up next" list rides along with the schedule refresh
    bool need_upcoming{false};
    std::vector<::espn::Upcoming> upcoming;
    bool upcoming_ok{false};
    bool no_event{false};
    GameSnapshot game;
    bool game_ok{false};
    // Favorite Teams mode: which cached entry this cycle refreshes or polls
    int fav_index{-1};
    uint32_t our_id{0};
  };
  void start_job_();
  static void worker_(void *arg);
  void run_job_();
  void apply_job_();
  bool fetch_schedule_(const ::espn::Team *team, Schedule &out);
  bool fetch_upcoming_(const ::espn::Team *team, ::espn::TeamSchedule &out);
  bool fetch_standings_(League league, uint32_t group, ::espn::Standings &out);
  bool fetch_points_(float lat, float lon, std::string &grid, int &status);
  bool fetch_weather_(const std::string &grid, ::nws::Weather &out);
  bool fetch_game_(const ::espn::Team *team, const Schedule &schedule, GameSnapshot &out);
  bool fetch_scan_(League league, Job &j);
  bool fetch_next_(League league, ::espn::LiveGame &out);
  bool live_mode_() const {
    return this->prefs2_.mode >= (uint8_t) Mode::LIVE_NFL && this->prefs2_.mode <= (uint8_t) Mode::LIVE_ANY;
  }
  // Mode 4 with at least one favorite set; with none it behaves like My Team.
  bool favorites_mode_() const { return this->prefs2_.mode == (uint8_t) Mode::FAVORITES && !this->fav_.empty(); }
  uint32_t our_id_() const;  // team id the snapshot is oriented around
  std::shared_ptr<http_request::HttpContainer> open_(const std::string &url, int *status = nullptr,
                                                    const char *accept = "application/json");
  void schedule_next_(uint32_t ms) { this->next_fetch_ms_ = millis() + ms; }
  uint32_t interval_for_phase_() const;
  void emit_(const ::espn::Splash &splash);
  void reset_game_();
  // A game poll came back clean: clear the miss count and start the clock over.
  void mark_good_poll_() {
    this->misses_ = 0;
    this->last_good_ms_ = millis() == 0 ? 1 : millis();
  }
  // True once a poll has actually put game data on the board this boot.
  bool ever_polled_good_() const { return this->last_good_ms_ != 0; }
  // Seconds since the last good poll. With nothing ever polled, report the
  // time since boot rather than 0: the board has had no update for its whole
  // uptime, and 0 would read to the app as "fresh". Callers that need to
  // phrase it for a person test ever_polled_good_() first.
  uint32_t stale_seconds_() const {
    if (this->last_good_ms_ == 0)
      return millis() / 1000;
    return (millis() - this->last_good_ms_) / 1000;
  }

  http_request::HttpRequestComponent *http_{nullptr};
  time::RealTimeClock *time_{nullptr};
  select::Select *team_select_{nullptr};
  select::Select *timezone_select_{nullptr};
  std::vector<std::function<void(const UpdateFields &)>> callbacks_;
  std::vector<std::function<void(const IdleFields &)>> idle_callbacks_;

  ESPPreferenceObject pref_;
  Prefs prefs_{};
  ESPPreferenceObject pref2_;
  Prefs2 prefs2_{};
  select::Select *mode_select_{nullptr};
  select::Select *favorite_selects_[4]{nullptr, nullptr, nullptr, nullptr};
  ESPPreferenceObject pref3_;
  Prefs3 prefs3_{};
  ESPPreferenceObject pref4_;
  Prefs4 prefs4_{};
  ESPPreferenceObject pref5_;
  Prefs5 prefs5_{};
  std::string favorite_option_(uint8_t slot) const;
  // Favorite Teams mode: one cached next-game card per set slot, in slot
  // order. The worker refreshes one entry per cycle; the main loop picks
  // which one the panel shows (favorites.h) and polls only that game.
  struct FavEntry {
    const ::espn::Team *team{nullptr};
    uint8_t slot{0};  // 1-4, the remote button
    Schedule sched;
    GameSnapshot game;
    uint32_t fetched_ms{0};  // last schedule attempt
    int64_t final_epoch{0};  // when the panel saw the game go final, 0 if it was fetched final
    bool stale{false};       // released after a final: fetch the next game
  };
  std::vector<FavEntry> fav_;
  int fav_shown_{-1};
  int64_t fav_shown_since_{0};  // epoch seconds
  int fav_pinned_{-1};
  bool fav_locked_{false};
  uint32_t fav_poll_ms_{0};
  void rebuild_favorites_(bool keep_cards);
  ::espn::FavRules fav_rules_() const;
  std::vector<::espn::FavGame> fav_games_() const;
  bool fav_choose_(int64_t now_epoch);  // true when the shown entry changed
  void start_fav_job_(uint32_t now);
  void apply_fav_job_(uint32_t now);
  uint32_t live_away_id_{0};      // the followed game's away team
  uint32_t live_started_ms_{0};   // when the current live game was picked
  uint32_t last_scan_ms_{0};      // when the last league scan was started
  uint32_t card_polled_ms_{0};    // last poll of a later/final/next card
  LiveShow live_show_{LiveShow::NONE};
  // The "my_team" fallback is up: the board shows the saved team's card
  // through the My team path and keeps scanning.
  bool team_fallback_{false};
  // From the last scan, kept so a next-game lookup can re-run the pick.
  std::vector<::espn::LiveGame> finals_;
  ::espn::LiveGame later_;
  size_t final_idx_{0};
  uint32_t final_since_ms_{0};
  // The league's next game this week, per league (index = League).
  struct NextCache {
    uint32_t fetched_ms{0};  // 0 = never
    uint32_t valid_ms{0};    // how long this answer stands
    bool found{false};
    ::espn::LiveGame game;
  };
  NextCache next_[2];
  uint8_t next_pending_{0};  // bit per league: look it up on the next cycle
  // Idle screens. idle_ is true while the mode has nothing to show and the
  // panel rotates through the owner's idle screens instead.
  bool idle_{false};
  int idle_screen_{-1};
  uint32_t idle_since_ms_{0};
  uint32_t idle_screen_ms_{0};  // when the current screen went up
  uint32_t idle_woke_ms_{0};    // start of the off-after-idle count
  uint32_t idle_tick_ms_{0};
  uint32_t idle_job_check_ms_{0};
  bool idle_dark_{false};       // the off timer switched the panel off
  bool idle_turning_off_{false};  // inside our own idle_off: not a manual off
  std::function<bool()> power_state_;
  // The owner picked a page by hand during idle: no rotation until the next
  // time idle is entered.
  bool idle_manual_{false};
  uint32_t idle_shown_ms_{0};   // when the rotation last switched pages
  std::string active_page_;
  std::string page_key_;        // last render of a page shown outside idle
  // Worker abandoned at WORKER_DEADLINE: no new worker of any kind before this.
  uint32_t worker_cooldown_until_{0};
  bool worker_cooling_() const { return (int32_t) (millis() - this->worker_cooldown_until_) < 0; }
  // Booted from the team cache and not yet confirmed by a game poll.
  bool cache_unconfirmed_{false};
  uint32_t wifi_no_clock_ms_{0};  // Wi-Fi up, clock not set, since when
  bool waiting_clock_{false};
  // Boot: until the first real content, the clock stands in (clock first).
  bool content_seen_{false};
  bool content_ready_{false};
  std::string idle_key_;        // what was last rendered, to skip repeats
  // The saved team's season schedule, whatever the mode: the countdown,
  // record and standings group come from it.
  ::espn::TeamSchedule team_sched_;
  const ::espn::Team *team_sched_team_{nullptr};
  uint32_t team_sched_ms_{0}, team_sched_try_ms_{0};
  ::espn::Standings standings_;
  uint32_t standings_key_{0};
  uint32_t standings_ms_{0}, standings_try_ms_{0};
  ::nws::Weather weather_;
  uint32_t weather_ms_{0}, weather_try_ms_{0};
  bool idle_wanted_() const;
  uint8_t idle_available_() const;
  const ::espn::Upcoming *team_next_game_(int64_t now_epoch) const;
  IdleFields idle_fields_(int screen);
  void render_idle_(bool show);
  void idle_tick_();
  bool start_idle_job_(uint32_t now);
  void apply_idle_job_(uint32_t now);
  void save_prefs5_() { this->pref5_.save(&this->prefs5_); }
  void mark_content_ready_();
  bool league_wanted_(League league) const;
  ::espn::NextState next_state_(int64_t now_epoch) const;
  void decide_live_(uint32_t now, const std::vector<::espn::LiveGame> &live);
  void show_live_card_(const ::espn::LiveGame &g, uint32_t now);
  void start_worker_();

  Schedule schedule_;
  uint32_t schedule_fetched_ms_{0};
  // Refresh Now: re-read the team endpoint on the next cycle, whatever
  // the timestamp says. Cleared once that read succeeds, so a refresh
  // that fails on a flaky network is retried rather than dropped.
  bool force_schedule_{false};
  std::vector<::espn::Upcoming> upcoming_;  // next games after the one on the board
  bool upcoming_due_{false};  // fetch the list on its own cycle, after the board has its game
  // demo playback
  bool demo_active_{false};
  uint8_t demo_step_{0};
  uint32_t demo_next_ms_{0};
  GameSnapshot demo_saved_game_, demo_saved_prev_;
  void demo_tick_();
  GameSnapshot game_;
  GameSnapshot prev_;
  uint32_t post_since_ms_{0};
  uint32_t next_fetch_ms_{0};
  uint8_t misses_{0};
  uint32_t last_good_ms_{0};  // millis() of the last successful game poll, 0 = never
  uint32_t busy_since_ms_{0};  // millis() when the worker task was started
  uint32_t worker_seq_{0};     // bumped when a worker is abandoned
  Job job_;
  uint32_t generation_{0};
  bool busy_{false};
  bool selects_published_{false};
  volatile bool job_done_{false};
};

class UpdateTrigger : public Trigger<const UpdateFields &> {
 public:
  explicit UpdateTrigger(GamedayComponent *parent) {
    parent->add_on_update_callback([this](const UpdateFields &f) { this->trigger(f); });
  }
};

class IdleTrigger : public Trigger<const IdleFields &> {
 public:
  explicit IdleTrigger(GamedayComponent *parent) {
    parent->add_on_idle_callback([this](const IdleFields &f) { this->trigger(f); });
  }
};

// Fires on the main loop for POST /gameday/action?do=<name>.
class ActionTrigger : public Trigger<std::string> {
 public:
  explicit ActionTrigger(GamedayComponent *parent) {
    parent->add_on_action_callback([this](std::string name) { this->trigger(std::move(name)); });
  }
};

}  // namespace gameday
}  // namespace esphome
