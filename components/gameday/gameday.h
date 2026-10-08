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
#include "esphome/core/helpers.h"
#include "esphome/core/component.h"
#include "esphome/core/preferences.h"

#include "espn_parse.h"
#include "favorites.h"
#include "startup.h"

namespace esphome {
namespace gameday {

using ::espn::GameSnapshot;
using ::espn::GameState;
using ::espn::League;
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
  // Other sports (football leaves these at their defaults).
  uint8_t sport{0};        // ::espn::Sport
  std::string situation;   // baseball: the count "2-1" while a half inning is on
                           // basketball: the playoff series "NY lead 2-1"
  int outs{-1};            // baseball: outs in the half inning, -1 when none
  uint8_t bases{0};        // baseball: 1 first, 2 second, 4 third
  bool highlight{false};   // the situation row stands out (bases loaded)
  int team_marks{0};       // soccer: red cards, drawn under each side's logo
  int opp_marks{0};
};

enum class SelectType : uint8_t { TEAM, TIMEZONE, MODE, FAVORITE };

// Favorite Teams mode holds up to this many teams. The WizMote's buttons 1-4
// jump to the first four.
static constexpr uint8_t FAV_MAX = 16;

// Allocates in PSRAM when there is some, else internal RAM. The favorite
// cards are ~1 KB each and internal heap is the panel's scarcest resource.
template<class T> class PsramAllocator : public RAMAllocator<T> {
 public:
  template<class U> struct rebind {
    using other = PsramAllocator<U>;
  };
  using is_always_equal = std::true_type;
  PsramAllocator() : RAMAllocator<T>(RAMAllocator<T>::NONE) {}
  template<class U> PsramAllocator(const PsramAllocator<U> &) : PsramAllocator() {}
};
template<class T, class U> bool operator==(const PsramAllocator<T> &, const PsramAllocator<U> &) { return true; }
template<class T, class U> bool operator!=(const PsramAllocator<T> &, const PsramAllocator<U> &) { return false; }

// LIVE (5) follows live games in the leagues of the live mask; modes 1-3 are
// the football-only live modes from before, kept for panels that saved them.
enum class Mode : uint8_t { MY_TEAM = 0, LIVE_NFL = 1, LIVE_NCAA = 2, LIVE_ANY = 3, FAVORITES = 4, LIVE = 5 };

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
  void set_today_only(bool on);
  // Live mode's leagues as keys ("nfl,mlb"); "" = the leagues you follow.
  void set_live_leagues(const std::string &keys);
  std::vector<League> live_leagues_() const;  // the leagues Live mode scans
  bool today_only() const { return (this->prefs5_.flags & FAV5_TODAY) != 0; }
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
  // The first real scoreboard since boot, or since the team last changed,
  // is on the board. The boot screen waits for it (boot.yaml, boot_hide).
  bool board_ready() const { return this->board_ready_; }
  // True while a boot screen hold that started at since_ms should go on
  // (startup.h). 0 means no hold is running.
  bool boot_hold(uint32_t since_ms) const { return ::espn::boot_hold(this->board_ready_, since_ms, millis()); }
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
  bool mark_setup_done_();
  void fire_action_(const std::string &name);
  void set_favorite_(uint8_t slot, uint8_t league, uint32_t id);
  // The whole list at once, in priority order (the page's favs= key).
  void set_favorites_(const std::vector<std::pair<uint8_t, uint32_t>> &list);
  void favorites_changed_(bool first_pick);
  uint8_t fav_league_(uint8_t i) const;  // i = 0..FAV_MAX-1
  uint32_t fav_id_(uint8_t i) const;     // 0 = empty
  void put_fav_(uint8_t i, uint8_t league, uint32_t id);  // no save
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
  static constexpr uint8_t FLAG_SETUP = 64;     // a team, mode or favorite has been picked at least once
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
  // Favorites 5-16 (1-4 stay in Prefs3, so older panels keep theirs) and the
  // settings that came with the longer list.
  struct Prefs5 {
    uint8_t fav_league[FAV_MAX - 4];
    uint32_t fav_id[FAV_MAX - 4];  // 0 = empty
    uint8_t flags;                 // FAV5_TODAY
    uint16_t live_mask;            // Live mode: 1 << League per league, 0 = the leagues you follow
  } __attribute__((packed));
  static constexpr uint8_t FAV5_TODAY = 1;  // idle playlist shows only today's games

  bool flag_(uint8_t f) const { return (this->prefs_.flags & f) != 0; }
  void set_flag_(uint8_t f, bool on);
  void save_prefs_();
  void publish_selects_();
  void publish_team_select_(const std::string &option);
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
    std::vector<League> scan_leagues;
    std::vector<::espn::LiveGame> live;
    bool scan_ok{false};
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
    // Soccer: the team's reads per competition (comps[0] the league's, then
    // its kCups in order), and the one cup this cycle reads (1.., -1 none).
    std::vector<Schedule> comps;
    int comp{-1};
    bool defer_game{false};  // first league read for this team: the cups decide before a poll
    bool need_game{false};   // the board has no game for the current schedule yet
    bool polled{false};      // a cup read polled the game it picked
    std::string done_event;  // a game seen to finish, passed over by the pick
  };
  void start_job_();
  static void worker_(void *arg);
  void run_job_();
  void apply_job_();
  bool fetch_schedule_(const ::espn::Team *team, Schedule &out, const char *comp_slug = nullptr);
  void run_cup_job_();
  void apply_cup_job_(uint32_t now);
  bool fetch_upcoming_(const ::espn::Team *team, std::vector<::espn::Upcoming> &out);
  bool fetch_game_(const ::espn::Team *team, const Schedule &schedule, GameSnapshot &out);
  bool fetch_live_games_(League league, std::vector<::espn::LiveGame> &out);
  bool live_mode_() const {
    return (this->prefs2_.mode >= (uint8_t) Mode::LIVE_NFL && this->prefs2_.mode <= (uint8_t) Mode::LIVE_ANY) ||
           this->prefs2_.mode == (uint8_t) Mode::LIVE;
  }
  // Mode 4 with at least one favorite set; with none it behaves like My Team.
  bool favorites_mode_() const { return this->prefs2_.mode == (uint8_t) Mode::FAVORITES && !this->fav_.empty(); }
  uint32_t our_id_() const;  // team id the snapshot is oriented around
  std::shared_ptr<http_request::HttpContainer> open_(const std::string &url);
  void schedule_next_(uint32_t ms) { this->next_fetch_ms_ = millis() + ms; }
  uint32_t interval_for_phase_() const;
  uint32_t linger_ms_() const;  // how long a final stays up before the next game
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
  // The team endpoint's answer for the saved team (startup.h), so a boot can
  // skip that read and go straight to the scoreboard.
  ESPPreferenceObject team_cache_pref_;
  bool team_cache_tried_{false};     // one look per boot
  bool cache_unconfirmed_{false};    // schedule_ came from the cache and no poll has confirmed it
  bool upcoming_after_poll_{false};  // booted from the cache: season list after the first board
  bool load_team_cache_(const ::espn::Team *team);
  void save_team_cache_(const ::espn::Team *team);
  bool board_ready_{false};
  uint32_t board_pending_ms_{0};  // millis() of the last team change, 0 = waiting since boot
  void mark_board_ready_();
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
    std::vector<Schedule> comps;  // soccer: reads per competition, as Job::comps
    int comp_next{-1};            // the cup read due next, -1 when none
  };
  using FavList = std::vector<FavEntry, PsramAllocator<FavEntry>>;
  FavList fav_;
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
  uint32_t live_away_id_{0};      // the followed live game's away team
  uint32_t live_started_ms_{0};   // when the current live game was picked,
                                  // or, in the fallback, when the last scan ran
  bool live_none_{false};         // last scan found nothing in progress
  // A live mode with nothing live: the board shows the saved team's card and
  // keeps scanning. The mode the owner picked does not change.
  bool live_fallback_{false};

  Schedule schedule_;
  uint32_t schedule_fetched_ms_{0};
  // Soccer: the saved team's reads per competition (as Job::comps), the cup
  // read due next (-1 when none) and the last game seen to finish.
  std::vector<Schedule> comps_;
  int comp_next_{-1};
  std::string done_event_;
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

// Fires on the main loop for POST /gameday/action?do=<name>.
class ActionTrigger : public Trigger<std::string> {
 public:
  explicit ActionTrigger(GamedayComponent *parent) {
    parent->add_on_action_callback([this](std::string name) { this->trigger(std::move(name)); });
  }
};

}  // namespace gameday
}  // namespace esphome
