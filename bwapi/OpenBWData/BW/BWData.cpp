#include "BWData.h"

#include "bwgame.h"
#include "actions.h"
#include "replay.h"
#include "sync.h"
#include "replay_saver.h"
#include "sync_server_asio_tcp.h"
#ifndef _WIN32
#include "sync_server_asio_local.h"
#include "sync_server_asio_posix_stream.h"

#include <sys/types.h>
#include <sys/stat.h>
#include <dirent.h>
#endif

#ifdef OPENBW_ENABLE_UI
#include "ui/ui.h"
#endif

#include <mutex>
#include <cstdio>
#include <type_traits>
#include <thread>
#include <chrono>
#include <algorithm>
#include <random>
#include <functional>
#include <cstdlib>
#include <cstring>
#include <condition_variable>

using bwgame::error;

//#ifdef OPENBW_ENABLE_UI
namespace bwgame {
namespace ui {
void log_str(a_string str) {
  printf("%s", str.c_str());
  fflush(stdout);
}

void fatal_error_str(a_string str){
  bwgame::error("%s", str);
}
}
}
//#endif

namespace BW {


#ifdef OPENBW_ENABLE_UI

struct thread_func_t {
  struct wrapper {
    virtual ~wrapper() {}
    virtual void call() = 0;
  };
  template<typename T>
  struct wrapper_t: wrapper {
    T f;
    wrapper_t(T f) : f(std::move(f)) {}
    virtual ~wrapper_t() {}
    virtual void call() override {
      f();
    }
  };
  std::unique_ptr<wrapper> wrapper_func;
  thread_func_t() {}
  thread_func_t(std::nullptr_t) {}
  template<typename F>
  thread_func_t(F f) {
    wrapper_func = std::make_unique<wrapper_t<F>>(std::move(f));
  }
  void operator()() {
    wrapper_func->call();
  }
  explicit operator bool() const {
    return (bool)wrapper_func;
  }
  thread_func_t& operator=(std::nullptr_t) {
    wrapper_func = nullptr;
    return *this;
  }
  template<typename F>
  thread_func_t& operator=(F f) {
    wrapper_func = std::make_unique<wrapper_t<F>>(std::move(f));
    return *this;
  }
};

bool ui_thread_enabled = false;

std::mutex ui_thread_mut;
std::condition_variable ui_thread_cv;
bool exit_ui_thread = false;
thread_func_t ui_thread_func;
std::thread ui_thread_main_thread;

void ui_thread_entry() {
  while (true) {
    std::unique_lock<std::mutex> l(ui_thread_mut);
    ui_thread_cv.wait(l, [&]{
      return ui_thread_func || exit_ui_thread;
    });
    if (exit_ui_thread) break;
    ui_thread_func();
    ui_thread_func = nullptr;
  }
  ui_thread_main_thread.join();
}

struct ui_thread_t {
  std::thread t;
  ui_thread_t() {
  }
  ui_thread_t(const ui_thread_t&) = delete;
  ui_thread_t(ui_thread_t&&) = default;
  ui_thread_t& operator=(const ui_thread_t&) {
    return *this;
  }
  ui_thread_t& operator=(ui_thread_t&&) {
    return *this;
  }
  template<typename F>
  ui_thread_t(F f) {
    if (ui_thread_enabled) {
      std::lock_guard<std::mutex> l(ui_thread_mut);
      ui_thread_func = std::move(f);
      ui_thread_cv.notify_one();
    } else {
      t = std::thread(std::forward<F>(f));
    }
  }
  void join() {
    if (ui_thread_enabled) {
      std::lock_guard<std::mutex> l(ui_thread_mut);
    } else {
      t.join();
    }
  }
};

struct ui_functions: bwgame::ui_functions {
  using bwgame::ui_functions::ui_functions;

  std::function<void(uint8_t*, size_t)> on_draw;

  virtual void draw_callback(uint8_t* data, size_t data_pitch) override {
    if (on_draw) on_draw(data, data_pitch);
  }
};

struct ui_wrapper {
  std::chrono::high_resolution_clock clock;
  std::chrono::high_resolution_clock::time_point last_update;
  ui_thread_t ui_thread;
  bool exit_thread = false;
  bool run_update = false;
  std::mutex mut;
  std::condition_variable cv;
  int game_speed = 0;
  bool window_closed = false;
  uint8_t* m_screen_buffer = nullptr;
  int screen_width = 0;
  int screen_height = 0;
  int screen_pos_x = 0;
  int screen_pos_y = 0;
  std::function<void(uint8_t*, size_t)> on_draw;
  bwgame::game_player get_player(bwgame::state& st) {
    bwgame::game_player player;
    player.set_st(st);
    return player;
  }
  ui_wrapper(bwgame::state& st, std::string mpq_path) {

    ui_thread = ui_thread_t([this, player = get_player(st), mpq_path]() mutable {
      std::unique_lock<std::mutex> l(mut);
      ui_functions ui(std::move(player));

      ui.exit_on_close = false;
      ui.global_volume = 0;
      auto load_data_file = bwgame::data_loading::data_files_directory(mpq_path.c_str());
      ui.load_data_file = [&](bwgame::a_vector<uint8_t>& data, bwgame::a_string filename) {
        load_data_file(data, std::move(filename));
      };
      ui.init();

      size_t screen_width = 800;
      size_t screen_height = 600;

      ui.resize(screen_width, screen_height);
      ui.screen_pos = {(int)ui.game_st.map_width / 2 - (int)screen_width / 2, (int)ui.game_st.map_height / 2 - (int)screen_height / 2};

      ui.on_draw = [this, &ui](uint8_t* data, size_t data_pitch) {
        this->m_screen_buffer = data;
        //this->screen_width = ui.screen_width;
        this->screen_width = data_pitch;
        this->screen_height = ui.screen_height;
        this->screen_pos_x = ui.screen_pos.x;
        this->screen_pos_y = ui.screen_pos.y;
        this->on_draw(data, data_pitch);
      };

      while (!exit_thread) {
        cv.wait_for(l, std::chrono::milliseconds(42), [&]{
          return run_update || exit_thread;
        });
        run_update = false;

        last_update = clock.now();
        ui.update();
        window_closed = ui.window_closed;
      }
    });

  }
  ~ui_wrapper() {
    exit_thread = true;
    cv.notify_all();
    ui_thread.join();
  }
  void update() {
    auto now = clock.now();
    if (now - last_update < std::chrono::milliseconds(40)) return;
    run_update = true;
    cv.notify_all();
  }
  bool closed() {
    return window_closed;
  }
  auto get_lock() {
    return std::unique_lock<std::mutex>(mut);
  }

  void set_on_draw(std::function<void(uint8_t*, size_t)> f) {
    on_draw = std::move(f);
  }
  int width() {
    return screen_width;
  }
  int height() {
    return screen_height;
  }
  int screen_x() {
    return screen_pos_x;
  }
  int screen_y() {
    return screen_pos_y;
  }
  uint8_t* screen_buffer() {
    return m_screen_buffer;
  }
};

struct draw_ui_wrapper {
  bwgame::game_player get_player(bwgame::state& st) {
    bwgame::game_player player;
    player.set_st(st);
    return player;
  }
  ui_functions ui;
  draw_ui_wrapper(bwgame::state& st, std::string mpq_path) : ui(get_player(st)) {
    ui.create_window = false;
    ui.draw_ui_elements = false;
    ui.exit_on_close = false;
    ui.global_volume = 0;
    auto load_data_file = bwgame::data_loading::data_files_directory(mpq_path.c_str());
    ui.load_data_file = [&](bwgame::a_vector<uint8_t>& data, bwgame::a_string filename) {
      load_data_file(data, std::move(filename));
    };
    ui.init();

    size_t screen_width = 800;
    size_t screen_height = 600;

    ui.resize(screen_width, screen_height);
    ui.screen_pos = {(int)ui.game_st.map_width / 2 - (int)screen_width / 2, (int)ui.game_st.map_height / 2 - (int)screen_height / 2};
  }
  std::tuple<int, int, uint32_t*> draw(int x, int y, int width, int height) {
    ui.screen_pos = {x, y};
    if (ui.screen_width != width || ui.screen_height != height) ui.resize(width, height);
    ui.update();
    return ui.get_rgba_buffer();
  }
};

#else
struct ui_wrapper {
  ui_wrapper(bwgame::state& st, std::string mpq_path) {}
  void update() {}
  bool closed() {
    return false;
  }
  auto get_lock() {
    return 0;
  }
  void set_on_draw(std::function<void(uint8_t*, size_t)> f) {
  }
  int width() {
    return 0;
  }
  int height() {
    return 0;
  }
  int screen_x() {
    return 0;
  }
  int screen_y() {
    return 0;
  }
  uint8_t* screen_buffer() {
    return nullptr;
  }
};
struct draw_ui_wrapper {
  draw_ui_wrapper(bwgame::state& st, std::string mpq_path) {}
  std::tuple<int, int, uint32_t*> draw(int x, int y, int width, int height) {
    return std::make_tuple(0, 0, (uint32_t*)nullptr);
  }
};

#endif

// Dual-host: which in-process viewer (bot) the CURRENT THREAD is dispatching for. Command
// encoders on viewer-less handles (Player, Unit) attribute through this; the dual launcher
// sets it once per bot dispatch thread. Single mode: always 0.
// sb-perf: initial-exec TLS (this library is loaded at program start). The guard is
// open-coded because this TU does not include BWAPI/Game.h, where BWAPI_TLS_IE and
// the rationale live: keep the two conditions identical if either changes.
#if defined(__ELF__) && !defined(SB_NO_TLS_IE)
thread_local int active_viewer __attribute__((tls_model("initial-exec"))) = 0;
#else
thread_local int active_viewer = 0;
#endif

void set_thread_viewer(int viewer) {
  active_viewer = viewer;
}

struct game_vars {
  int local_player_id = -1;
  bool is_replay = false;

  // Per-viewer identity (dual-host: one in-process bot per entry; single mode uses [0]).
  struct viewer_vars_t {
    bool left_game = false;
    std::string character_name;
  };
  std::array<viewer_vars_t, 2> viewers;

  // Dual-host lobby-end picked-race snapshot (the one GameImpl::createMultiPlayerGame takes
  // itself; the dual lobby runs below the BWAPI layer, so BOTH viewers' GameImpls consume
  // this shared copy at match start). Invalid outside dual games.
  std::array<int, 12> dual_picked_races{};
  bool dual_races_valid = false;

  int game_type = 0;
  bool game_type_melee = false;

  std::array<int, 7> game_speeds_frame_times{{ 167, 111, 83, 67, 56, 48, 42 }};
  int game_speed = 6;

  std::string map_filename;
  std::string set_map_filename;

  std::array<int, 12> map_player_races{};
  std::array<int, 12> map_player_controllers{};

  bool is_multi_player = false;

  std::unordered_map<std::string, std::string> override_env_var;
};

void g_global_init_if_necessary(const bwgame::global_state& global_st, std::string mpq_path);

struct game_setup_helper_t {
  bwgame::state& st;
  game_vars& vars;
  bwgame::replay_functions& replay_funcs;
  bwgame::sync_functions& sync_funcs;

  bwgame::a_vector<uint8_t> scenario_chk_data;

  bwgame::sync_server_noop noop_server;
  bwgame::sync_server_asio_tcp tcp_server;
#ifndef _WIN32
  bwgame::sync_server_asio_local local_server;
#else
  bwgame::sync_server_noop local_server;
#endif
#ifndef _WIN32
  bwgame::sync_server_asio_posix_stream file_server;
#else
  bwgame::sync_server_noop file_server;
#endif

  int server_n = 0;

  std::string env(std::string name, std::string def) {
    auto i = vars.override_env_var.find(name);
    if (i != vars.override_env_var.end()) return i->second;
    const char* s = std::getenv(name.c_str());
    if (!s) return def;
    return s;
  }

  void create_single_player_game(std::function<void()> setup_function) {
    using bwgame::error;

    for (auto& v : vars.viewers) v.left_game = false;
    vars.dual_races_valid = false;
    vars.is_multi_player = false;

    auto& filename = vars.set_map_filename;

    auto& global_st = *st.global;
    auto& game_st = *st.game;
    game_st = bwgame::game_state();
    st = bwgame::state();
    st.global = &global_st;
    st.game = &game_st;

    g_global_init_if_necessary(global_st, env("OPENBW_MPQ_PATH", "."));

    scenario_chk_data.clear();

    std::string ext;
    size_t dot_pos = filename.rfind('.');
    if (dot_pos != std::string::npos) {
      ext = filename.substr(dot_pos + 1);
      for (auto& v : ext) v |= 0x20;
    }

    vars.map_player_controllers = {};
    vars.map_player_races = {};

    server_n = 0;

    if (ext == "rep") {

      replay_funcs.replay_st = bwgame::replay_state();
      replay_funcs.load_replay_file(filename, true, &scenario_chk_data);

      vars.is_replay = true;
      vars.local_player_id = -1;

      vars.game_type = replay_funcs.replay_st.game_type;
      vars.game_type_melee = vars.game_type == 2;

    } else {

      auto name = sync_funcs.sync_st.local_client->name;
      auto* save_replay = sync_funcs.sync_st.save_replay;
      sync_funcs.action_st = bwgame::action_state();
      sync_funcs.sync_st = bwgame::sync_state();
      sync_funcs.sync_st.local_client->name = std::move(name);
      sync_funcs.sync_st.save_replay = save_replay;
      if (save_replay) *save_replay = bwgame::replay_saver_state();
      bwgame::game_load_functions load_funcs(st);

      (bwgame::data_loading::mpq_file<>(filename))(scenario_chk_data, "staredit/scenario.chk");

      if (save_replay) {
        save_replay->map_data = scenario_chk_data.data();
        save_replay->map_data_size = scenario_chk_data.size();
      }

      load_funcs.load_map_data(scenario_chk_data.data(), scenario_chk_data.size(), [&]() {

        vars.game_type = vars.game_type_melee ? 2 : 10;
        sync_funcs.sync_st.game_type_melee = vars.game_type_melee;
        if (vars.game_type_melee) {
          load_funcs.setup_info.victory_condition = 1;
          load_funcs.setup_info.starting_units = 2;
          load_funcs.setup_info.resource_type = 1;
        }
        sync_funcs.sync_st.setup_info = &load_funcs.setup_info;

        while (!sync_funcs.sync_st.game_started) {
          sync_funcs.sync(noop_server);
          vars.local_player_id = sync_funcs.sync_st.local_client->player_slot;
          if (vars.local_player_id != -1) {
            for (auto& v : st.players) {
              if (v.controller == bwgame::player_t::controller_open || v.controller == bwgame::player_t::controller_computer) {
                  v.controller = bwgame::player_t::controller_computer;
              }
            }
          }
          setup_function();
        }

        // Park clientless "ghost" slots before the map-load tail runs. The lobby start
        // (sync.h start_game) promotes every leftover open/computer slot to an occupied
        // player with no client behind it; any ghost the melee slot shuffle seats on a
        // start location is then handed a full idle starting base by create_starting_units,
        // and razing its last building mid-game flips the melee "Opponents at most 0"
        // victory trigger — ending a single-player game early with a win. As
        // controller_neutral a ghost still counts for the opponents condition (so the real
        // player is not instantly victorious in an otherwise empty game) but receives no
        // starting units and runs no melee triggers, keeping single-player games truly solo.
        if (vars.local_player_id != -1) {
          for (auto& v : st.players) {
            if ((int)(&v - st.players.data()) == vars.local_player_id) continue;
            if (v.controller == bwgame::player_t::controller_occupied) {
              v.controller = bwgame::player_t::controller_neutral;
            }
          }
        }

        sync_funcs.sync_st.setup_info = nullptr;
      });

      vars.is_replay = false;
    }

    vars.map_filename = filename;
  }

  // Dual-host (ENGINE_OPT_DUALHOST.md): ONE state, ONE process, TWO local clients — the
  // in-process replacement for a two-process LOCAL_AUTO game. The secondary registers under
  // the uid a peer launched with OPENBW_CLIENT_INDEX=2 would have had, so lobby seating and
  // action-application order match the two-process reference by construction.
  void create_dual_player_game(std::function<void()> setup_function) {
    using bwgame::error;

    for (auto& v : vars.viewers) v.left_game = false;
    vars.dual_races_valid = false;
    vars.is_multi_player = true;

    auto& filename = vars.set_map_filename;

    auto& global_st = *st.global;
    auto& game_st = *st.game;
    game_st = bwgame::game_state();
    st = bwgame::state();
    st.global = &global_st;
    st.game = &game_st;

    g_global_init_if_necessary(global_st, env("OPENBW_MPQ_PATH", "."));

    scenario_chk_data.clear();

    vars.map_player_controllers = {};
    vars.map_player_races = {};

    server_n = 0;

    auto name = sync_funcs.sync_st.local_client->name;
    auto* save_replay = sync_funcs.sync_st.save_replay;
    sync_funcs.action_st = bwgame::action_state();
    sync_funcs.sync_st = bwgame::sync_state();
    sync_funcs.sync_st.local_client->name = std::move(name);
    sync_funcs.sync_st.save_replay = save_replay;
    if (save_replay) *save_replay = bwgame::replay_saver_state();

    // Match the LOCAL_AUTO reference's action latency exactly — actions schedule at frame
    // N+latency, so a different value is a different game.
    sync_funcs.sync_st.latency = 3;

    // Name the local viewer "p1" to pair with the secondary "p2" below: otherwise the local
    // viewer records no name and the replay header / in-game browser show it as "?". Cosmetic
    // only -- a client NAME feeds neither the uid (seed material is (seed, client index)) nor the
    // simulation, so the game stays byte-identical (gated by the seed-906 digest).
    sync_funcs.sync_st.local_client->name = "p1";

    secondary_client = sync_funcs.add_local_secondary_client("p2", 2);

    bwgame::game_load_functions load_funcs(st);

    (bwgame::data_loading::mpq_file<>(filename))(scenario_chk_data, "staredit/scenario.chk");

    if (save_replay) {
      save_replay->map_data = scenario_chk_data.data();
      save_replay->map_data_size = scenario_chk_data.size();
    }

    load_funcs.load_map_data(scenario_chk_data.data(), scenario_chk_data.size(), [&]() {

      vars.game_type = vars.game_type_melee ? 2 : 10;
      sync_funcs.sync_st.game_type_melee = vars.game_type_melee;
      if (vars.game_type_melee) {
        load_funcs.setup_info.victory_condition = 1;
        load_funcs.setup_info.starting_units = 2;
        load_funcs.setup_info.resource_type = 1;
      }
      sync_funcs.sync_st.setup_info = &load_funcs.setup_info;

      while (!sync_funcs.sync_st.game_started) {
        sync_funcs.sync(noop_server);
        vars.local_player_id = sync_funcs.sync_st.local_client->player_slot;
        setup_function();
      }
      vars.local_player_id = sync_funcs.sync_st.local_client->player_slot;

      // Park clientless ghost slots exactly as single-player creation does, keeping BOTH
      // local clients' seats (same early-victory bug otherwise — see the single-player note).
      int p1_slot = sync_funcs.sync_st.local_client->player_slot;
      int p2_slot = secondary_client ? secondary_client->player_slot : -1;
      for (auto& v : st.players) {
        int idx = (int)(&v - st.players.data());
        if (idx == p1_slot || idx == p2_slot) continue;
        if (v.controller == bwgame::player_t::controller_occupied) {
          v.controller = bwgame::player_t::controller_neutral;
        }
      }

      sync_funcs.sync_st.setup_info = nullptr;
    });

    // The peer's start acknowledgement: the in-game applier discards every action from a
    // client whose game_started flag is unset (sync.h:842), and the flag is set only by an
    // id_game_started action FROM that client — which a peer process sends when its own side
    // starts. Inject the same for the in-process secondary.
    {
      uint8_t ack[1] = { (uint8_t)bwgame::sync_messages::id_game_started };
      input_action(1, ack, 1);
    }

    // Lobby-end picked-race snapshot for the viewers' GameImpls (see game_vars): without it
    // PlayerImpl::getRace reports unseen enemies as Unknown — a bot-visible difference the
    // two-process reference never shows (races are lobby-known), and the root cause of the
    // PvZ dual divergence (matchup-branched openings fork on race=Unknown).
    for (size_t i = 0; i != vars.dual_picked_races.size(); ++i)
      vars.dual_picked_races[i] = (int)sync_funcs.sync_st.picked_races.at(i);
    vars.dual_races_valid = true;

    vars.is_replay = false;
    vars.map_filename = filename;
  }

  // Secondary-client lobby race change: the same id_set_race action a peer process's
  // set_local_client_race would put on the wire, attributed to client 2.
  void set_secondary_race(int race) {
    if (!secondary_client) return;
    uint8_t buf[2] = { (uint8_t)bwgame::sync_messages::id_set_race, (uint8_t)race };
    input_action(1, buf, 2);
  }

  // Secondary-client lobby seating: the id_occupy_slot action a peer's switchToPlayer sends.
  void set_secondary_occupy_slot(int n) {
    if (!secondary_client) return;
    uint8_t buf[2] = { (uint8_t)bwgame::sync_messages::id_occupy_slot, (uint8_t)n };
    input_action(1, buf, 2);
  }

  void create_multi_player_game(std::function<void()> setup_function) {
    using bwgame::error;

    for (auto& v : vars.viewers) v.left_game = false;
    vars.dual_races_valid = false;
    vars.is_multi_player = true;

    auto& filename = vars.set_map_filename;

    auto& global_st = *st.global;
    auto& game_st = *st.game;
    game_st = bwgame::game_state();
    st = bwgame::state();
    st.global = &global_st;
    st.game = &game_st;

    g_global_init_if_necessary(global_st, env("OPENBW_MPQ_PATH", "."));

    scenario_chk_data.clear();

    std::string ext;
    size_t dot_pos = filename.rfind('.');
    if (dot_pos != std::string::npos) {
      ext = filename.substr(dot_pos + 1);
      for (auto& v : ext) v |= 0x20;
    }

    vars.map_player_controllers = {};
    vars.map_player_races = {};

    server_n = 0;

    bool is_replay = ext == "rep";

    auto name = sync_funcs.sync_st.local_client->name;
    auto* save_replay = sync_funcs.sync_st.save_replay;
    if (is_replay) save_replay = nullptr;
    sync_funcs.action_st = bwgame::action_state();
    sync_funcs.sync_st = bwgame::sync_state();
    sync_funcs.sync_st.local_client->name = std::move(name);
    sync_funcs.sync_st.save_replay = save_replay;
    if (save_replay) *save_replay = bwgame::replay_saver_state();
    sync_funcs.sync_st.latency = 3;
    bwgame::game_load_functions load_funcs(st);

    if (!is_replay) {
      (bwgame::data_loading::mpq_file<>(filename))(scenario_chk_data, "staredit/scenario.chk");

      if (save_replay) {
        save_replay->map_data = scenario_chk_data.data();
        save_replay->map_data_size = scenario_chk_data.size();
      }
    }

    std::string default_lan_mode = "LOCAL_AUTO";
#ifdef _WIN32
    default_lan_mode = "TCP";
#endif
    std::string lan_mode = env("OPENBW_LAN_MODE", default_lan_mode);
    for (auto& v : lan_mode) v &= ~0x20;
    if (lan_mode == "TCP") server_n = 1;
    else if (lan_mode == "LOCAL" || lan_mode=="LOCAL_FD" || lan_mode == "LOCAL_AUTO") server_n = 2;
    else if (lan_mode == "FD" || lan_mode == "FILE") server_n = 3;

    std::string listen_hostname = env("OPENBW_TCP_LISTEN_HOSTNAME", "0.0.0.0");
    int listen_port = std::atoi(env("OPENBW_TCP_LISTEN_PORT", "6112").c_str());
    std::string connect_hostname = env("OPENBW_TCP_CONNECT_HOSTNAME", "127.0.0.1");
    int connect_port = std::atoi(env("OPENBW_TCP_CONNECT_PORT", "6112").c_str());

    auto func = [&]() {
      if (server_n == 1) {
        tcp_server.bind(listen_hostname.c_str(), listen_port);
        tcp_server.connect(connect_hostname.c_str(), connect_port);
      } else if (server_n == 2) {
#ifndef _WIN32
        if (lan_mode == "LOCAL_AUTO") {
          std::string directory = env("OPENBW_LOCAL_AUTO_DIRECTORY", "");
          if (directory.empty()) directory = "/tmp/openbw";
          while (!directory.empty() && *std::prev(directory.end()) == '/') directory.erase(std::prev(directory.end()));
          mkdir(directory.c_str(), S_IRWXU | S_IRWXG | S_IRWXO);
          chmod(directory.c_str(), S_IRWXU | S_IRWXG | S_IRWXO);
          std::string path = directory + "/" + sync_funcs.sync_st.local_client->uid.str() + ".socket";
          local_server.bind(path.c_str());
          DIR* d = opendir(directory.c_str());
          if (d) {
            dirent* e;
            std::string fn;
            while ((e = readdir(d))) {
              if (!strcmp(e->d_name, ".")) continue;
              if (!strcmp(e->d_name, "..")) continue;
              fn = directory + "/" + e->d_name;
              struct stat s;
              if (!stat(fn.c_str(), &s) && S_ISSOCK(s.st_mode)) {
                  if (!local_server.try_connect(fn)) {
                  unlink(fn.c_str());
                }
              }
            }
            closedir(d);
          }
        } else if (lan_mode == "LOCAL_FD") {
          std::string fd_str = env("OPENBW_LOCAL_FD", "");
          if (fd_str.empty()) error("OPENBW_LAN_MODE is LOCAL_FD but OPENBW_LOCAL_FD not specified");
          local_server.assign(std::atoi(fd_str.c_str()));
        } else {
          std::string path = env("OPENBW_LOCAL_PATH", "");
          if (path.empty()) error("OPENBW_LAN_MODE is LOCAL but OPENBW_LOCAL_PATH not specified");
          for (int i = 0;; ++i) {
            if (local_server.try_bind(path.c_str())) break;
            if (local_server.try_connect(path.c_str())) break;
            if (i == 10) {
              local_server.connect(path.c_str());
              break;
            }
          }
        }
#else
        error("OPENBW_LAN_MODE %s not supported on Windows", lan_mode);
#endif
      } else if (server_n == 3) {
#ifndef _WIN32
        int fd_read = -1;
        int fd_write = -1;
        if (lan_mode == "FD") {
            std::string fd_read_str = env("OPENBW_FD_READ", "");
          std::string fd_write_str = env("OPENBW_FD_WRITE", "");
          if (fd_read_str.empty()) error("OPENBW_LAN_MODE is FD but OPENBW_FD_READ not specified");
          if (fd_write_str.empty()) error("OPENBW_LAN_MODE is FD but OPENBW_FD_WRITE not specified");
          fd_read = std::atoi(fd_read_str.c_str());
          fd_write = std::atoi(fd_write_str.c_str());
        } else {
          // The files provided here are presumably named pipes; ignore SIGPIPE
          // in this case so that each process can exit gracefully at the end of
          // the game
          struct sigaction sa;
          memset(&sa, 0, sizeof(sa));
          sa.sa_handler = SIG_IGN;
          sigaction(SIGPIPE, &sa, NULL);

          std::string file_read = env("OPENBW_FILE_READ", "");
          std::string file_write = env("OPENBW_FILE_WRITE", "");
          if (file_read.empty()) error("OPENBW_LAN_MODE is FILE but OPENBW_FILE_READ not specified");
          if (file_write.empty()) error("OPENBW_LAN_MODE is FILE but OPENBW_FILE_WRITE not specified");
          fd_read = open(file_read.c_str(), O_RDWR);
          if (fd_read == -1) error("open %s failed: %s", file_read.c_str(), std::strerror(errno));
          fd_write = open(file_write.c_str(), O_WRONLY);
          if (fd_write == -1) error("open %s failed: %s", file_write.c_str(), std::strerror(errno));
        }
        file_server.open(fd_read, fd_write);
#else
        error("OPENBW_LAN_MODE %s not supported on Windows", lan_mode);
#endif
      }

      vars.game_type = vars.game_type_melee ? 2 : 10;
      sync_funcs.sync_st.game_type_melee = vars.game_type_melee;
      if (vars.game_type_melee) {
        load_funcs.setup_info.victory_condition = 1;
        load_funcs.setup_info.starting_units = 2;
        load_funcs.setup_info.resource_type = 1;
      }
      sync_funcs.sync_st.setup_info = &load_funcs.setup_info;

      while (!sync_funcs.sync_st.game_started) {
        sync();
        setup_function();
      }

      sync_funcs.sync_st.setup_info = nullptr;
    };

    if (is_replay) {

      replay_funcs.replay_st = bwgame::replay_state();
      replay_funcs.load_replay_file(filename, true, &scenario_chk_data);

      vars.is_replay = true;
      vars.local_player_id = -1;

      vars.game_type = replay_funcs.replay_st.game_type;
      vars.game_type_melee = vars.game_type == 2;

      func();

    } else {
      vars.is_replay = false;
      load_funcs.load_map_data(scenario_chk_data.data(), scenario_chk_data.size(), [&]() {
        func();
      });
    }

    vars.map_filename = filename;
  }

  template<typename server_T>
  void leave_game(server_T& server, int viewer) {
    if (!vars.viewers[viewer].left_game) {
      vars.viewers[viewer].left_game = true;
      // Dual-host (secondary_client set): a leave only marks THIS viewer's session over —
      // nothing is sent into the shared session. Two-process (task #35 defect fix): the
      // concede goes through the NORMAL scheduled-action lane — exactly like any bot
      // command — so BOTH sims execute the {87,0} at the same deterministic frame and the
      // leaver's own sim marks itself left/defeated. The old path (send_leave_game +
      // final_sync) executed only on the peer, at arrival time, and the peer's
      // post-action cleanup killed the connection — fabricated {87,6} drops then let a
      // DEFEATED side declare itself victorious (dual-battery seed-606 double-win; the
      // task-#29 tail drift). The launcher's end grace keeps the sync pumping so the
      // scheduled leave lands in the leaver's sim after its own gameOver flag.
      if (viewer == 0 && !vars.is_replay && !secondary_client) {
        const uint8_t leave_action[2] = { 87, 0 };
        input_action(0, leave_action, 2);
      }
    }

  }

  void sync() {
    if (server_n == 0) sync_funcs.sync(noop_server);
    else if (server_n == 1) sync_funcs.sync(tcp_server);
    else if (server_n == 2) sync_funcs.sync(local_server);
    else if (server_n == 3) sync_funcs.sync(file_server);
    vars.local_player_id = sync_funcs.sync_st.local_client->player_slot;
  }

  void start_game() {
    if (server_n == 0) sync_funcs.start_game(noop_server);
    else if (server_n == 1) sync_funcs.start_game(tcp_server);
    else if (server_n == 2) sync_funcs.start_game(local_server);
    else if (server_n == 3) sync_funcs.start_game(file_server);
  }
  void switch_to_slot(int n) {
    if (sync_funcs.sync_st.local_client->player_slot == n) return;
    if (server_n == 0) sync_funcs.switch_to_slot(noop_server, n);
    else if (server_n == 1) sync_funcs.switch_to_slot(tcp_server, n);
    else if (server_n == 2) sync_funcs.switch_to_slot(local_server, n);
    else if (server_n == 3) sync_funcs.switch_to_slot(file_server, n);
  }
  void set_name(const std::string& name) {
    if (sync_funcs.sync_st.local_client->name == name) return;
    sync_funcs.set_local_client_name(name.c_str());
  }

  int connected_player_count() {
    return sync_funcs.connected_player_count();
  }

  void set_race(int race) {
    int n = sync_funcs.sync_st.local_client->player_slot;
    if (n == -1 || st.players.at(n).race == (bwgame::race_t)race) return;
    if (server_n == 0) sync_funcs.set_local_client_race(noop_server, (bwgame::race_t)race);
    else if (server_n == 1) sync_funcs.set_local_client_race(tcp_server, (bwgame::race_t)race);
    else if (server_n == 2) sync_funcs.set_local_client_race(local_server, (bwgame::race_t)race);
    else if (server_n == 3) sync_funcs.set_local_client_race(file_server, (bwgame::race_t)race);
  }

  void input_action(int viewer, const uint8_t* data, size_t size) {
    if (viewer == 1) {
      // Dual-host: attribute the secondary in-process bot's commands to ITS sync client so
      // scheduling (frame N+latency, uid order) matches the two-process reference exactly.
      if (!secondary_client) bwgame::error("input_action: viewer 1 without a secondary client");
      if (server_n == 0) sync_funcs.input_action_for(noop_server, secondary_client, data, size);
      else if (server_n == 1) sync_funcs.input_action_for(tcp_server, secondary_client, data, size);
      else if (server_n == 2) sync_funcs.input_action_for(local_server, secondary_client, data, size);
      else if (server_n == 3) sync_funcs.input_action_for(file_server, secondary_client, data, size);
      return;
    }
    if (server_n == 0) sync_funcs.input_action(noop_server, data, size);
    else if (server_n == 1) sync_funcs.input_action(tcp_server, data, size);
    else if (server_n == 2) sync_funcs.input_action(local_server, data, size);
    else if (server_n == 3) sync_funcs.input_action(file_server, data, size);
  }

  void next_frame() {
    if (server_n == 0) sync_funcs.bwapi_compatible_next_frame(noop_server);
    else if (server_n == 1) sync_funcs.bwapi_compatible_next_frame(tcp_server);
    else if (server_n == 2) sync_funcs.bwapi_compatible_next_frame(local_server);
    else if (server_n == 3) sync_funcs.bwapi_compatible_next_frame(file_server);
  }

  void leave_game(int viewer = 0) {
    if (server_n == 0) leave_game(noop_server, viewer);
    else if (server_n == 1) leave_game(tcp_server, viewer);
    else if (server_n == 2) leave_game(local_server, viewer);
    else if (server_n == 3) leave_game(file_server, viewer);
  }

  // Dual-host: the second in-process bot's sync client (nullptr in single mode).
  bwgame::sync_state::client_t* secondary_client = nullptr;

  bwgame::sync_state::client_t* client_for_viewer(int viewer) {
    if (viewer == 1 && secondary_client) return secondary_client;
    return sync_funcs.sync_st.local_client;
  }

  void create_unit(const bwgame::unit_type_t* unit_type, bwgame::xy pos, int owner) {
    if (server_n == 0) sync_funcs.create_unit(noop_server, unit_type, pos, owner);
    else if (server_n == 1) sync_funcs.create_unit(tcp_server, unit_type, pos, owner);
    else if (server_n == 2) sync_funcs.create_unit(local_server, unit_type, pos, owner);
    else if (server_n == 3) sync_funcs.create_unit(file_server, unit_type, pos, owner);
  }

  void kill_unit(bwgame::unit_t* u) {
    if (server_n == 0) sync_funcs.kill_unit(noop_server, u);
    else if (server_n == 1) sync_funcs.kill_unit(tcp_server, u);
    else if (server_n == 2) sync_funcs.kill_unit(local_server, u);
    else if (server_n == 3) sync_funcs.kill_unit(file_server, u);
  }

  void remove_unit(bwgame::unit_t* u) {
    if (server_n == 0) sync_funcs.remove_unit(noop_server, u);
    else if (server_n == 1) sync_funcs.remove_unit(tcp_server, u);
    else if (server_n == 2) sync_funcs.remove_unit(local_server, u);
    else if (server_n == 3) sync_funcs.remove_unit(file_server, u);
  }

  void send_custom_action(const void* data, size_t size) {
    if (server_n == 0) sync_funcs.send_custom_action(noop_server, data, size);
    else if (server_n == 1) sync_funcs.send_custom_action(tcp_server, data, size);
    else if (server_n == 2) sync_funcs.send_custom_action(local_server, data, size);
    else if (server_n == 3) sync_funcs.send_custom_action(file_server, data, size);
  }

};

template<typename F>
struct openbwapi_functions: F {
  game_vars& vars;

  template<typename... args_T>
  openbwapi_functions(game_vars& vars, args_T&&... args) : F(std::forward<args_T>(args)...), vars(vars) {}

};

struct openbwapi_impl {
  game_vars& vars;
  bwgame::state& st;
  openbwapi_functions<bwgame::state_functions> funcs;
  openbwapi_functions<bwgame::replay_functions> replay_funcs;
  openbwapi_functions<bwgame::sync_functions> sync_funcs;
  game_setup_helper_t game_setup_helper{st, vars, replay_funcs, sync_funcs};
  std::function<void(const char*)> print_text_callback;
  std::unique_ptr<ui_wrapper> ui;
  std::unique_ptr<draw_ui_wrapper> draw_ui;
  using clock_t = std::chrono::high_resolution_clock;
  clock_t clock;
  clock_t::time_point last_frame;
  std::function<void (uint8_t *, size_t)> on_draw;
  bool on_draw_changed = false;
  bool speed_override = false;
  int speed_override_value = 0;
  int screen_x = 0;
  int screen_y = 0;

  openbwapi_impl(game_vars& vars, bwgame::state& st, bwgame::action_state& action_st, bwgame::replay_state& replay_st, bwgame::sync_state& sync_st) : vars(vars), st(st), funcs(vars, st), replay_funcs(vars, st, action_st, replay_st), sync_funcs(vars, st, action_st, sync_st) {
    auto speed = game_setup_helper.env("OPENBW_GAME_SPEED", "");
    if (!speed.empty()) {
      speed_override = true;
      speed_override_value = std::atoi(speed.c_str());
    }
  }

  bool ui_enabled = false;

  void enable_ui() {
    if (ui_enabled) return;
    auto str = game_setup_helper.env("OPENBW_ENABLE_UI", "1");
    for (auto& v : str) {
      if (v >= 'a' && v <= 'z') v &= ~0x20;
    }
    if (str != "0" && str != "OFF" && str != "NO" && str != "FALSE" && str != "N") {
      ui_enabled = true;
    }
  }
  void disable_ui() {
    ui = nullptr;
    ui_enabled = false;
  }

  // SB_STATE_DIGEST_EVERY=N: per-frame live state digests, printed after each frame advance.
  // Same walk as tools/analysis/state_digest.h (kept in sync by hand — deterministic engine
  // order, raw fixed-point, RNG) so live games and replay re-simulation share digest semantics.
  // The cross-mode state-equality probe for the dual-host gates. Print-only, off unless set.
  void maybe_print_state_digest() {
    static const int every = [] {
      const char* e = std::getenv("SB_STATE_DIGEST_EVERY");
      return e ? std::atoi(e) : 0;
    }();
    if (every <= 0 || vars.is_replay) return;
    if (st.current_frame % every != 0) return;
    uint64_t h = 1469598103934665603ull;
    auto add = [&](uint64_t value) {
      for (int i = 0; i < 8; ++i) {
        h ^= (value >> (i * 8)) & 0xff;
        h *= 1099511628211ull;
      }
    };
    add((uint64_t)st.current_frame);
    int units[12];
    for (int slot = 0; slot < 12; ++slot) {
      units[slot] = 0;
      add((uint64_t)st.current_minerals[slot]);
      add((uint64_t)st.current_gas[slot]);
      for (auto& level : st.upgrade_levels.at(slot)) add((uint64_t)level);
      for (auto researched : st.tech_researched.at(slot)) add((uint64_t)(researched ? 1 : 0));
      for (auto& unit : st.player_units[slot]) {
        ++units[slot];
        add((uint64_t)unit.unit_type->id);
        add((uint64_t)(int64_t)unit.sprite->position.x);
        add((uint64_t)(int64_t)unit.sprite->position.y);
        add((uint64_t)(int64_t)unit.hp.raw_value);
        add((uint64_t)(int64_t)unit.shield_points.raw_value);
        add((uint64_t)(int64_t)unit.energy.raw_value);
        add((uint64_t)(unit.order_type ? (int)unit.order_type->id : -1));
        add((uint64_t)unit.ground_weapon_cooldown);
        add((uint64_t)unit.spell_cooldown);
      }
    }
    add((uint64_t)st.lcg_rand_state);
    std::printf("SBDIG f=%d h=%016llx rng=%08x n0=%d n1=%d\n", st.current_frame,
                (unsigned long long)h, (unsigned)st.lcg_rand_state, units[0], units[1]);
    std::fflush(stdout);
  }

  void next_frame() {
    if (!ui && ui_enabled) {
      ui = std::make_unique<ui_wrapper>(st, game_setup_helper.env("OPENBW_MPQ_PATH", "."));
    }
    if (ui) {
      auto l = ui->get_lock();
      if (vars.is_replay) {
        if (replay_funcs.is_done()) {
          game_setup_helper.leave_game();
        } else {
          replay_funcs.next_frame();
        }
      } else {
        game_setup_helper.next_frame();
      }
    } else {
      if (vars.is_replay) {
        if (replay_funcs.is_done()) {
          game_setup_helper.leave_game();
        } else {
          replay_funcs.next_frame();
        }
       } else {
        game_setup_helper.next_frame();
      }
    }
    maybe_print_state_digest();

    if (ui) {
      if (on_draw_changed && on_draw) {
        on_draw_changed = false;
        ui->set_on_draw([this](uint8_t* data, size_t data_pitch) {
          this->on_draw(data, data_pitch);
        });
      }
      ui->update();
      if (ui->closed()) {
        game_setup_helper.leave_game();
      }

      auto now = clock.now();
      auto speed = std::chrono::milliseconds(speed_override ? speed_override_value : vars.game_speeds_frame_times.back());
      if (speed > std::chrono::milliseconds(0)) {
        last_frame += speed;
        auto frame_t = now - last_frame;
        if (frame_t > speed * 8) {
          last_frame = now - speed * 8;
          frame_t = speed * 8;
        }
        if (frame_t < speed) {
          std::this_thread::sleep_for(speed - frame_t);
        }
      }
    }
  }

  std::tuple<int, int, uint32_t*> draw_game_screen(int x, int y, int width, int height) {
    if (!draw_ui) {
      draw_ui = std::make_unique<draw_ui_wrapper>(st, game_setup_helper.env("OPENBW_MPQ_PATH", "."));
    }
    return draw_ui->draw(x, y, width, height);
  }
};

template<typename T>
struct init_safe_global {
  typename std::aligned_storage<sizeof(T), alignof(T)>::type buf;
  bool inited;
  init_safe_global() {
    **this;
  }
  ~init_safe_global() {
    (**this).~T();
  }
  T& operator*() {
    if (!inited) {
      new ((T*)&buf) T{};
      inited = true;
    }
    return *(T*)&buf;
  }
};

init_safe_global<bwgame::global_state> g_global_st;
bool global_inited = false;
init_safe_global<std::mutex> global_init_mut;
void g_global_init(std::string mpq_path) {
  if (global_inited) return;
  std::lock_guard<std::mutex> l(*global_init_mut);
  if (global_inited) return;
  bwgame::global_init(*g_global_st, bwgame::data_loading::data_files_directory(mpq_path.c_str()));
  global_inited = true;
}

void g_global_init_if_necessary(const bwgame::global_state& global_st, std::string mpq_path) {
  if ((void*)&global_st == (void*)&g_global_st.buf) {
    g_global_init(std::move(mpq_path));
  }
}

struct full_state {
  bwgame::global_state& global_st = *g_global_st;
  bwgame::game_state game_st;
  bwgame::state st;
  bwgame::action_state action_st;
  bwgame::replay_state replay_st;
  bwgame::sync_state sync_st;
  bwgame::replay_saver_state replay_saver_st;
  full_state() {
    st.global = &global_st;
    st.game = &game_st;
    sync_st.save_replay = &replay_saver_st;
  }
};

struct GameOwner_impl {
  full_state fst;
  bwgame::state& st;
  bwgame::action_state& action_st;
  const bwgame::game_state& game_st;
  game_vars vars;
  openbwapi_impl impl;

  GameOwner_impl() : st(fst.st), action_st(fst.action_st), game_st(fst.game_st), impl(vars, fst.st, fst.action_st, fst.replay_st, fst.sync_st) {}

};


void sacrificeThreadForUI(std::function<void()> f)
{
#ifdef OPENBW_ENABLE_UI
  ui_thread_enabled = true;
  ui_thread_main_thread = std::thread([f = std::move(f)]{
    f();
    exit_ui_thread = true;
    ui_thread_cv.notify_all();
  });
  ui_thread_entry();
#else
  f();
#endif
}

GameOwner::GameOwner()
{
  impl = std::make_unique<GameOwner_impl>();
}

GameOwner::~GameOwner()
{
}

Game GameOwner::getGame()
{
  return {&impl->impl};
}

Game GameOwner::getGame(int viewer)
{
  return {&impl->impl, viewer};
}

void GameOwner::setPrintTextCallback(std::function<void (const char*)> func)
{
  impl->impl.print_text_callback = func;
}

struct snapshot_impl {
  bwgame::state st;
  bwgame::optional<bwgame::action_state> action_st;
  game_vars vars;
};

Snapshot::Snapshot()
{
}

Snapshot::Snapshot(Snapshot&& n)
{
  impl = std::move(n.impl);
}

Snapshot::~Snapshot()
{
}

Snapshot& Snapshot::operator=(Snapshot&& n)
{
  impl = std::move(n.impl);
  return *this;
}


void Game::overrideEnvVar(std::string var, std::string value)
{
  impl->vars.override_env_var[std::move(var)] = std::move(value);
}

int Game::g_LocalHumanID() const {
  return impl->game_setup_helper.client_for_viewer(viewer_index)->player_slot;
}

Player Game::getPlayer(int n) const
{
  return {n, impl};
}

int Game::gameType() const
{
  return impl->vars.game_type;
}

int Game::Latency() const
{
  return 0;
}

int Game::ReplayHead_frameCount() const
{
  return impl->vars.is_replay ? impl->replay_funcs.replay_st.end_frame : 0;
}

int Game::MouseX() const
{
  return 0;
}

int Game::MouseY() const
{
  return 0;
}

int Game::ScreenX() const
{
  if (impl->ui) return impl->ui->screen_x();
  return 0;
}

int Game::ScreenY() const
{
  if (impl->ui) return impl->ui->screen_y();
  return 0;
}

int Game::screenWidth() const
{
  if (impl->ui) return impl->ui->width();
  return 0;
}

int Game::screenHeight() const
{
  if (impl->ui) return impl->ui->height();
  return 0;
}

void Game::setScreenPosition(int x, int y)
{

}

int Game::NetMode() const
{
  return 0;
}

bool Game::isGamePaused() const
{
  return false;
}

bool Game::InReplay() const
{
  return impl->vars.is_replay;
}

void Game::QueueCommand(const void* buf, size_t size)
{
  if (!impl->vars.is_replay) impl->game_setup_helper.input_action(viewer_index,(const uint8_t*)buf, size);
}

void Game::leaveGame()
{
  impl->game_setup_helper.leave_game(viewer_index);
}

bool Game::gameClosed() const
{
  if (impl->ui) return impl->ui->closed();
  return false;
}

u32 Game::ReplayVision() const
{
  throw std::runtime_error("ReplayVision");
}

void Game::setReplayVision(u32)
{
  throw std::runtime_error("setReplayVision");
}

void Game::setGameSpeedModifiers(int n, int value)
{
  impl->vars.game_speeds_frame_times.at(n) = value;
}

void Game::setAltSpeedModifiers(int n, int value)
{

}

void Game::setFrameSkip(int value)
{

}

int Game::getLatencyFrames() const
{
  return impl->sync_funcs.sync_st.latency;
}

int Game::GameSpeed() const
{
  return impl->vars.game_speed;
}

int Game::gameSpeedModifiers(int speed) const
{
  return impl->vars.game_speeds_frame_times.at(speed);
}

int Game::lastTurnFrame() const
{
  return impl->funcs.st.current_frame; // fixme?
}

bool Game::setMapFileName(const std::string& filename)
{
  impl->vars.set_map_filename = filename;
  return true;
}

int Game::elapsedTime() const
{
  return impl->funcs.st.current_frame;
}

int Game::countdownTimer() const
{
  throw std::runtime_error("countdownTimer?");
}

void Game::setReplayRevealAll(bool)
{
  throw std::runtime_error("setReplayRevealAll");
}

u32 Game::ReplayHead_gameSeed_randSeed() const
{
  throw std::runtime_error("ReplayHead_gameSeed_randSeed");
}

int Game::mapTileSize_x() const
{
  return impl->funcs.game_st.map_tile_width;
}

int Game::mapTileSize_y() const
{
  return impl->funcs.game_st.map_tile_height;
}

const char* Game::mapFileName() const
{
  return impl->vars.map_filename.c_str();
}

const char* Game::mapTitle() const
{
  return impl->funcs.game_st.scenario_name.c_str();
}

activeTile Game::getActiveTile(int tile_x, int tile_y) const
{
  auto& tile = impl->funcs.st.tiles[impl->funcs.tile_index({int(tile_x * 32u), int(tile_y * 32u)})];
  activeTile r;
  r.bVisibilityFlags = tile.visible;
  r.bExploredFlags = tile.explored;
  r.bTemporaryCreep = tile.flags & bwgame::tile_t::flag_temporary_creep;
  r.bCurrentlyOccupied = tile.flags & bwgame::tile_t::flag_occupied;
  r.bHasCreep = tile.flags & bwgame::tile_t::flag_has_creep;
  return r;
}

bool Game::buildable(int tile_x, int tile_y) const
{
  auto& tile = impl->funcs.st.tiles[impl->funcs.tile_index({int(tile_x * 32u), int(tile_y * 32u)})];
  return ~tile.flags & bwgame::tile_t::flag_unbuildable;
}

bool Game::walkable(int walk_x, int walk_y) const
{
  return impl->funcs.is_walkable({walk_x * 8, walk_y * 8});
}

bool Game::hasCreep(int tile_x, int tile_y) const
{
  return impl->funcs.tile_has_creep({int(tile_x * 32u), int(tile_y * 32u)});
}

bool Game::isOccupied(int tile_x, int tile_y) const
{
  auto& tile = impl->funcs.st.tiles[impl->funcs.tile_index({int(tile_x * 32u), int(tile_y * 32u)})];
  return tile.flags & bwgame::tile_t::flag_occupied;
}

int Game::groundHeight(int tile_x, int tile_y) const
{
  auto& tile = impl->funcs.st.tiles[impl->funcs.tile_index({int(tile_x * 32u), int(tile_y * 32u)})];
  int r = 0;
  if (tile.flags & bwgame::tile_t::flag_very_high) r += 1;
  if (tile.flags & bwgame::tile_t::flag_middle) r += 2;
  if (tile.flags & bwgame::tile_t::flag_high) r += 4;
  return r;
  //return impl->funcs.get_ground_height_at({int(tile_x * 32u), int(tile_y * 32u)});
}

u8 Game::bVisibilityFlags(int tile_x, int tile_y) const
{
  return ~impl->funcs.tile_visibility({int(tile_x * 32u), int(tile_y * 32u)});
}

u8 Game::bExploredFlags(int tile_x, int tile_y) const
{
  return ~impl->funcs.tile_explored({int(tile_x * 32u), int(tile_y * 32u)});
}

Unit Game::getUnit(size_t index) const
{
  return {impl->funcs.get_unit(index), impl};
}

Bullet Game::getBullet(size_t index) const
{
  return {impl->funcs.st.bullets_container.try_get(index), impl};
}

std::array<int, 12> Game::bRaceInfo() const
{
  return impl->vars.map_player_races;
}

std::array<int, 12> Game::bOwnerInfo() const
{
  return impl->vars.map_player_controllers;
}

const char* Game::forceNames(size_t n) const
{
  return impl->funcs.game_st.forces[n].name.c_str();
}

template<typename T>
using itobj = typename std::conditional<std::is_same<T, Unit>::value, decltype(bwgame::state::visible_units), decltype(bwgame::state::active_bullets)>::type::iterator;

BulletIterator Game::BulletNodeTable_begin() const
{
  BulletIterator r;
  r.obj.as<itobj<Bullet>>() = impl->funcs.st.active_bullets.begin();
  r.impl = impl;
  return r;
}

BulletIterator Game::BulletNodeTable_end() const
{
  BulletIterator r;
  r.obj.as<itobj<Bullet>>() = impl->funcs.st.active_bullets.end();
  r.impl = impl;
  return r;
}

UnitIterator Game::UnitNodeList_VisibleUnit_begin() const
{
  UnitIterator r;
  r.obj.as<itobj<Unit>>() = impl->funcs.st.visible_units.begin();
  r.impl = impl;
  return r;
}

UnitIterator Game::UnitNodeList_VisibleUnit_end() const
{
  UnitIterator r;
  r.obj.as<itobj<Unit>>() = impl->funcs.st.visible_units.end();
  r.impl = impl;
  return r;
}

UnitIterator Game::UnitNodeList_HiddenUnit_begin() const
{
  UnitIterator r;
  r.obj.as<itobj<Unit>>() = impl->funcs.st.hidden_units.begin();
  r.impl = impl;
  return r;
}

UnitIterator Game::UnitNodeList_HiddenUnit_end() const
{
  UnitIterator r;
  r.obj.as<itobj<Unit>>() = impl->funcs.st.hidden_units.end();
  r.impl = impl;
  return r;
}

UnitIterator Game::UnitNodeList_ScannerSweep_begin() const
{
  UnitIterator r;
  r.obj.as<itobj<Unit>>() = impl->funcs.st.map_revealer_units.begin();
  r.impl = impl;
  return r;
}

UnitIterator Game::UnitNodeList_ScannerSweep_end() const
{
  UnitIterator r;
  r.obj.as<itobj<Unit>>() = impl->funcs.st.map_revealer_units.end();
  r.impl = impl;
  return r;
}


void Game::setCharacterName(const std::string& name)
{
  impl->vars.viewers[viewer_index].character_name = name;
  if (viewer_index == 0) impl->game_setup_helper.set_name(name);
}

void Game::setGameTypeMelee()
{
  impl->vars.game_type_melee = true;
}

void Game::setGameTypeUseMapSettings()
{
  impl->vars.game_type_melee = false;
}

void Game::createSinglePlayerGame(std::function<void()> setupFunction)
{
  impl->game_setup_helper.create_single_player_game(std::move(setupFunction));
}

void Game::createDualPlayerGame(std::function<void()> setupFunction)
{
  impl->game_setup_helper.create_dual_player_game(std::move(setupFunction));
}

void Game::setDualSecondaryRace(int race)
{
  impl->game_setup_helper.set_secondary_race(race);
}

int Game::dualSecondarySlot() const
{
  auto* c = impl->game_setup_helper.secondary_client;
  return c ? c->player_slot : -1;
}

int Game::dualPickedRace(int slot) const
{
  if (!impl->vars.dual_races_valid) return -1;
  if (slot < 0 || slot >= (int)impl->vars.dual_picked_races.size()) return -1;
  return impl->vars.dual_picked_races[slot];
}

bool Game::dualSecondaryUidSortsFirst() const
{
  auto* sec = impl->game_setup_helper.secondary_client;
  auto* loc = impl->sync_funcs.sync_st.local_client;
  if (!sec || !loc) return false;
  return sec->uid < loc->uid;
}

void Game::dualLocalOccupySlot(int n)
{
  impl->game_setup_helper.switch_to_slot(n);
}

void Game::dualSecondaryOccupySlot(int n)
{
  impl->game_setup_helper.set_secondary_occupy_slot(n);
}

void Game::createMultiPlayerGame(std::function<void()> setupFunction)
{
  impl->game_setup_helper.create_multi_player_game(std::move(setupFunction));
}

void Game::startGame()
{
  impl->game_setup_helper.start_game();
}

void Game::switchToSlot(int n)
{
  impl->game_setup_helper.switch_to_slot(n);
}

int Game::connectedPlayerCount()
{
  return impl->game_setup_helper.connected_player_count();
}

UnitFinderIterator Game::UnitOrderingX() const
{
  return {impl->funcs.st.unit_finder_x.data()};
}

UnitFinderIterator Game::UnitOrderingY() const
{
  return {impl->funcs.st.unit_finder_y.data()};
}

size_t Game::UnitOrderingCount() const
{
  return impl->funcs.st.unit_finder_x.size();
}

bool Game::triggersCanAllowGameplayForPlayer(int n) const
{
  for (int i = 0; i != 8; ++i) {
    if (impl->funcs.st.players[i].controller != bwgame::player_t::controller_occupied) continue;
    for (auto& rt : impl->funcs.st.running_triggers[i]) {
      if (rt.flags & 8) continue;
      auto& t = *rt.t;
      if (~rt.flags & 1) {
        bool can_execute = true;
        for (auto& c : t.conditions) {
          if (c.type == 0) break;
          if (c.type == 23) can_execute = false;
        }
        if (!can_execute) continue;
      }
      auto player_can_match = [&](int owner, int player) {
        if (!impl->funcs.player_slot_active(n)) return false;
        if (player < 12) return n == player; // player index
        switch (player) {
        case 12: return false; // no players
        case 13: return n == owner; // current player
        case 14: return owner != n; // enemy
        case 15: return owner != n; // ally
        case 16: return owner != n; // neutral
        case 17: return true; // all players
        case 18: return n < 8 && impl->funcs.st.players[n].force == 1;
        case 19: return n < 8 && impl->funcs.st.players[n].force == 2;
        case 20: return n < 8 && impl->funcs.st.players[n].force == 3;
        case 21: return n < 8 && impl->funcs.st.players[n].force == 4;
        case 26: return owner != n; // non allied victory
        default: return false;
        }
      };
      for (auto& a : t.actions) {
        if (a.type == 0) break;
        if (a.flags & 2) continue;
        if (a.type == 11 || a.type == 44) { // create unit
          if (player_can_match(i, a.group_n)) return true;
        } else if (a.type == 48) { // give units
          if (player_can_match(i, a.group2_n)) return true;
        }
      }
    }
  }
  return false;
}

int Game::getInstanceNumber() const
{
  return 0;
}

bool Game::getScenarioChk(std::vector<char>& data) const
{
  auto& src = impl->game_setup_helper.scenario_chk_data;
  data.resize(src.size());
  std::copy(src.begin(), src.end(), data.begin());
  return !data.empty();
}

bool Game::gameOver() const
{
  auto done = [&]() {
    int n = impl->game_setup_helper.client_for_viewer(viewer_index)->player_slot;
    if (n != -1 && (impl->funcs.player_won(n) || impl->funcs.player_defeated(n))) return true;
    return false;
  };
  return impl->vars.viewers[viewer_index].left_game || done();
}

void Game::printText(const char* str) const
{
  if (!impl->print_text_callback) return;
  // Dual-host: both bots share one stdout, so tag each line with the dispatching viewer
  // (thread-bound) — the runner splits the stream back into per-player logs.
  if (impl->game_setup_helper.secondary_client) {
    std::string tagged = "V" + std::to_string(active_viewer) + "|" + str;
    impl->print_text_callback(tagged.c_str());
    return;
  }
  impl->print_text_callback(str);
}

void Game::nextFrame()
{
  impl->next_frame();
}

void Game::setGUI(bool enabled)
{
  if (enabled) impl->enable_ui();
  else impl->disable_ui();
}

void Game::enableCheats() const
{
  impl->funcs.st.cheats_enabled = true;
}

void Game::saveReplay(const std::string& filename)
{
  if (impl->sync_funcs.sync_st.save_replay) {
    bwgame::replay_saver_functions replay_saver_funcs(*impl->sync_funcs.sync_st.save_replay);
    bwgame::data_loading::file_writer<> w(filename.c_str());
    replay_saver_funcs.save_replay(impl->st.current_frame, w);
  }
}

std::tuple<int, int, void*> Game::GameScreenBuffer()
{
  if (impl->ui) return std::make_tuple(impl->ui->width(), impl->ui->height(), impl->ui->screen_buffer());
  return std::make_tuple(0, 0, nullptr);
}

void Game::setOnDraw(std::function<void (uint8_t*, size_t)> onDraw)
{
  impl->on_draw = onDraw;
  impl->on_draw_changed = true;
}

std::tuple<int, int, uint32_t*> Game::drawGameScreen(int x, int y, int width, int height)
{
  return impl->draw_game_screen(x, y, width, height);
}

size_t Game::regionCount() const
{
  return impl->funcs.game_st.regions.regions.size();
}

Region Game::getRegion(size_t index) const
{
  return {index, impl};
}

Region Game::getRegionAt(int x, int y) const
{
  if (!impl->funcs.is_in_map_bounds({x, y})) return {};
  return {impl->funcs.get_region_at({x, y})->index, impl};
}

Unit Game::createUnit(int owner, int unitType, int x, int y)
{
  if (!impl->vars.is_multi_player) {
    return {impl->funcs.trigger_create_unit(impl->funcs.get_unit_type((bwgame::UnitTypes)unitType), {x, y}, owner), impl};
  } else {
    impl->game_setup_helper.create_unit(impl->funcs.get_unit_type((bwgame::UnitTypes)unitType), {x, y}, owner);
    return {nullptr, impl};
  }
}

void Game::killUnit(Unit u)
{
  if (!impl->vars.is_multi_player) {
    if (u.u) impl->funcs.kill_unit(u.u);
  } else {
    impl->game_setup_helper.kill_unit(u.u);
  }
}

void Game::removeUnit(Unit u)
{
  if (!impl->vars.is_multi_player) {
    if (u.u) {
      impl->funcs.hide_unit(u.u);
      impl->funcs.kill_unit(u.u);
    }
  } else {
    impl->game_setup_helper.remove_unit(u.u);
  }
}

Snapshot Game::saveSnapshot()
{
  auto v = std::make_unique<snapshot_impl>();
  v->st = bwgame::copy_state(impl->st);
  if (impl->vars.is_replay) v->action_st = bwgame::copy_state(impl->sync_funcs.action_st, impl->st, v->st);
  v->vars = impl->vars;
  Snapshot r;
  r.impl = std::move(v);
  return r;
}

void Game::loadSnapshot(const Snapshot& snapshot)
{
  auto& v = snapshot.impl;
  impl->vars = v->vars;
  impl->st = bwgame::copy_state(v->st);
  if (impl->vars.is_replay) impl->sync_funcs.action_st = bwgame::copy_state(*v->action_st, v->st, impl->st);

  if (impl->sync_funcs.sync_st.save_replay) {
    impl->sync_funcs.sync_st.save_replay = nullptr;
  }
}

void Game::setRandomSeed(uint32_t value)
{
  impl->st.lcg_rand_state = value;
}

void Game::disableTriggers()  
{
  impl->st.trigger_timer = -1;
}

void Game::sendCustomAction(const void *data, size_t size)
{
  impl->game_setup_helper.send_custom_action(data, size);
}

void Game::setCustomActionCallback(std::function<void(int, const char* data, size_t size)> callback)
{
  impl->game_setup_helper.sync_funcs.on_custom_action = [callback = std::move(callback)](int player, auto& r) {
    size_t n = r.left();
    callback(player, (const char*)r.get_n(n), n);
  };
}

int Player::playerColorIndex() const
{
  return impl->funcs.st.players.at(owner).color;
}

const char* Player::szName() const
{
  if (impl->vars.is_replay) {
    return impl->replay_funcs.replay_st.player_name.at(owner).c_str();
  } else {
    return impl->sync_funcs.sync_st.player_names.at(owner).c_str();
  }
}

int Player::nRace() const
{
  return (int)impl->funcs.st.players.at(owner).race;
}

int Player::pickedRace() const
{
  return (int)impl->sync_funcs.sync_st.picked_races.at(owner);
}

int Player::nType() const
{
  return (int)impl->funcs.st.players.at(owner).controller;
}

int Player::nTeam() const
{
  return (int)impl->funcs.st.players.at(owner).force;
}

int Player::playerAlliances(int n) const
{
  return impl->funcs.st.alliances.at(owner).at(n);
}

Position Player::startPosition() const
{
  return {(s16)impl->funcs.game_st.start_locations.at(owner).x, (s16)impl->funcs.game_st.start_locations.at(owner).y};
}

int Player::PlayerVictory() const
{
  return impl->st.players.at(owner).victory_state;
}

int Player::currentUpgradeLevel(int n) const
{
  return impl->funcs.player_upgrade_level(owner, (bwgame::UpgradeTypes)n);
}

int Player::maxUpgradeLevel(int n) const
{
  return impl->funcs.player_max_upgrade_level(owner, (bwgame::UpgradeTypes)n);
}

bool Player::techResearched(int n) const
{
  return impl->funcs.player_has_researched(owner, (bwgame::TechTypes)n);
}

bool Player::techAvailable(int n) const
{
  return impl->funcs.player_tech_available(owner, (bwgame::TechTypes)n);
}

bool Player::upgradeInProgress(int n) const
{
  return impl->funcs.player_is_upgrading(owner, (bwgame::UpgradeTypes)n);
}

bool Player::techResearchInProgress(int n) const
{
  return impl->funcs.player_is_researching(owner, (bwgame::TechTypes)n);
}

bool Player::unitAvailability(int n) const
{
  return impl->funcs.game_st.unit_type_allowed.at(owner).at((bwgame::UnitTypes)n);
}

int Player::minerals() const
{
  return impl->funcs.st.current_minerals.at(owner);
}

int Player::gas() const
{
  return impl->funcs.st.current_gas.at(owner);
}

int Player::cumulativeMinerals() const
{
  return impl->funcs.st.total_minerals_gathered.at(owner);
}

int Player::cumulativeGas() const
{
  return impl->funcs.st.total_gas_gathered.at(owner);
}

int Player::suppliesAvailable(int n) const
{
  return impl->funcs.st.supply_available.at(owner).at(n).raw_value;
}

int Player::suppliesMax(int n) const
{
  return 400; // fixme
}

int Player::suppliesUsed(int n) const
{
  return impl->funcs.st.supply_used.at(owner).at(n).raw_value;
}

int Player::unitCountsDead(int n) const
{
  return 0; // fixme
}

int Player::unitCountsKilled(int n) const
{
  return 0; // fixme
}

int Player::unitCountsAll(int n) const
{
  return impl->funcs.st.unit_counts.at(owner).at((bwgame::UnitTypes)n);
}

int Player::allUnitsLost() const
{
  return 0; // fixme
}

int Player::allBuildingsLost() const
{
  return 0; // fixme
}

int Player::allFactoriesLost() const
{
  return 0; // fixme
}

int Player::allUnitsKilled() const
{
  return 0; // fixme
}

int Player::allBuildingsRazed() const
{
  return 0; // fixme
}

int Player::allFactoriesRazed() const
{
  return 0; // fixme
}

int Player::allUnitScore() const
{
  return 0; // fixme
}

int Player::allKillScore() const
{
  return 0; // fixme
}

int Player::allBuildingScore() const
{
  return 0; // fixme
}

int Player::allRazingScore() const
{
  return 0; // fixme
}

int Player::customScore() const
{
  return 0; // fixme
}

u32 Player::playerVision() const
{
  return impl->funcs.st.shared_vision.at(owner);
}

int Player::downloadStatus() const
{
  return 100;
}

// See PlayerMirrorFingerprint in BWData.h. Deliberately implemented by calling the same
// accessors PlayerImpl::updateData() calls, from inside this library: the fingerprint then
// reads exactly what the recompute reads by construction, and the only thing that changes
// is that ~700 crossings of the library boundary become one. Any accessor added to
// PlayerImpl::updateData() must be added here too, or the skip goes stale — grep
// PlayerImpl.cpp when in doubt (the unit-side analogue of this rule caught the TvT
// remaining_build_time miss).
void Player::mirrorFingerprint(PlayerMirrorFingerprint* dst, bool needCapabilities, bool needScores) const
{
  std::memset(dst, 0, sizeof(*dst));

  dst->color = playerColorIndex();
  dst->race  = nRace();
  dst->type  = nType();

  dst->minerals            = minerals();
  dst->gas                 = gas();
  dst->cumulative_minerals = cumulativeMinerals();
  dst->cumulative_gas      = cumulativeGas();

  for (int i = 0; i != PlayerMirrorFingerprint::kRaces; ++i) {
    dst->supplies_available[i] = suppliesAvailable(i);
    dst->supplies_max[i]       = suppliesMax(i);
    dst->supplies_used[i]      = suppliesUsed(i);
  }

  // Fill only what the branch in force will actually consume. The hidden-player branches
  // zero these arrays instead of reading them, so fingerprinting them there costs ~700 reads
  // per player per frame to detect changes in values nobody looks at. (Measured: filling
  // unconditionally made BW::Player::unitAvailability 21% MORE expensive than not skipping
  // at all, because the recompute had been skipping hidden players and the fill was not.)
  if (needCapabilities) {
    for (int i = 0; i != PlayerMirrorFingerprint::kUpgradeTypes; ++i) {
      dst->upgrade_level[i]       = currentUpgradeLevel(i);
      dst->max_upgrade_level[i]   = maxUpgradeLevel(i);
      dst->upgrade_in_progress[i] = upgradeInProgress(i) ? 1 : 0;
    }

    for (int i = 0; i != PlayerMirrorFingerprint::kTechTypes; ++i) {
      dst->tech_researched[i]  = techResearched(i) ? 1 : 0;
      dst->tech_available[i]   = techAvailable(i) ? 1 : 0;
      dst->tech_in_progress[i] = techResearchInProgress(i) ? 1 : 0;
    }

    for (int i = 0; i != PlayerMirrorFingerprint::kUnitTypes; ++i)
      dst->unit_available[i] = unitAvailability(i) ? 1 : 0;
  } else {
    // The hidden branch still reads currentUpgradeLevel for units we have seen.
    for (int i = 0; i != PlayerMirrorFingerprint::kUpgradeTypes; ++i)
      dst->upgrade_level[i] = currentUpgradeLevel(i);
  }

  if (!needScores) return;

  // unitCountsDead/Killed are `return 0` in this engine (BWData.cpp, marked fixme) — index
  // independent, so one probe covers the whole array and 456 constant reads per player per
  // frame disappear. If they are ever implemented per type, SB_PLAYER_MIRROR_SKIP=verify
  // reports it immediately as a deadUnitCount/killedUnitCount diff; that gate is the reason
  // this shortcut is allowed rather than guessed at.
  dst->units_dead_probe   = unitCountsDead(0);
  dst->units_killed_probe = unitCountsKilled(0);

  dst->all_units_lost      = allUnitsLost();
  dst->all_buildings_lost  = allBuildingsLost();
  dst->all_factories_lost  = allFactoriesLost();
  dst->all_units_killed    = allUnitsKilled();
  dst->all_buildings_razed = allBuildingsRazed();
  dst->all_factories_razed = allFactoriesRazed();

  dst->unit_score     = allUnitScore();
  dst->kill_score     = allKillScore();
  dst->building_score = allBuildingScore();
  dst->razing_score   = allRazingScore();
  dst->custom_score   = customScore();
}

void Player::setRace(int race)
{
  if (!impl->vars.is_multi_player) {
    if (impl->sync_funcs.sync_st.initial_slot_races.at(owner) == (bwgame::race_t)5) {
      impl->funcs.st.players.at(owner).race = (bwgame::race_t)race;
    }
  } else {
    if (owner == impl->sync_funcs.sync_st.local_client->player_slot) {
      impl->game_setup_helper.set_race(race);
    }
  }
}

void Player::closeSlot()
{
  auto c = impl->funcs.st.players.at(owner).controller;
  if (c == bwgame::player_t::controller_occupied || c == bwgame::player_t::controller_computer || c == bwgame::player_t::controller_open) {
    impl->funcs.st.players.at(owner).controller = bwgame::player_t::controller_closed;
  }
}

void Player::openSlot()
{
  if (impl->funcs.st.players.at(owner).controller == bwgame::player_t::controller_closed) {
    impl->funcs.st.players.at(owner).controller = bwgame::player_t::controller_open;
  }
}

void Player::setUpgradeLevel(int upgrade, int level)
{
  bwgame::sync_functions::dynamic_writer<> w;
  w.template put<uint8_t>(210);
  w.template put<uint8_t>(1);
  w.template put<uint8_t>(0);
  w.template put<uint8_t>(owner);
  w.template put<uint8_t>(upgrade);
  w.template put<int8_t>(level);
  impl->game_setup_helper.input_action(active_viewer, w.data(), w.size());
}

void Player::setResearched(int tech, bool researched)
{
  bwgame::sync_functions::dynamic_writer<> w;
  w.template put<uint8_t>(210);
  w.template put<uint8_t>(1);
  w.template put<uint8_t>(1);
  w.template put<uint8_t>(owner);
  w.template put<uint8_t>(tech);
  w.template put<uint8_t>(researched);
  impl->game_setup_helper.input_action(active_viewer, w.data(), w.size());
}

void Player::setMinerals(int value)
{
  bwgame::sync_functions::dynamic_writer<> w;
  w.template put<uint8_t>(210);
  w.template put<uint8_t>(1);
  w.template put<uint8_t>(2);
  w.template put<uint8_t>(owner);
  w.template put<int32_t>(value);
  impl->game_setup_helper.input_action(active_viewer, w.data(), w.size());
}

void Player::setGas(int value)
{
  bwgame::sync_functions::dynamic_writer<> w;
  w.template put<uint8_t>(210);
  w.template put<uint8_t>(1);
  w.template put<uint8_t>(3);
  w.template put<uint8_t>(owner);
  w.template put<int32_t>(value);
  impl->game_setup_helper.input_action(active_viewer, w.data(), w.size());
}

Unit::operator bool() const
{
  return u != nullptr;
}

size_t Unit::getIndex() const
{
  return u->index;
}

u16 Unit::getUnitID() const
{
  return impl->funcs.get_unit_id(u).raw_value;
}

Position Unit::position() const
{
  return {(s16)u->position.x, (s16)u->position.y};
}

bool Unit::hasSprite() const
{
  return u->sprite != nullptr;
}

int Unit::visibilityFlags() const
{
  return u->sprite->visibility_flags;
}

int Unit::unitType() const
{
  return (int)u->unit_type->id;
}

bool Unit::statusFlag(int flag) const
{
  return impl->funcs.u_status_flag(u, (bwgame::unit_t::status_flags_t)flag);
}

u32 Unit::visibilityStatus() const
{
  return u->detected_flags;
}

bool Unit::movementFlag(int flag) const
{
  return impl->funcs.u_movement_flag(u, flag);
}

int Unit::orderID() const
{
  return u->order_type ? (int)u->order_type->id : 0;
}

int Unit::groundWeaponCooldown() const
{
  return u->ground_weapon_cooldown;
}

int Unit::airWeaponCooldown() const
{
  return u->air_weapon_cooldown;
}

int Unit::playerID() const
{
  return u->owner;
}

size_t Unit::buildQueueSlot() const
{
  return 0;
}

int Unit::buildQueue(size_t index) const
{
  return index < u->build_queue.size() ? (int)u->build_queue[index]->id : 228;
}

bool Unit::fighter_inHanger() const
{
  return impl->funcs.unit_is_fighter(u) ? u->fighter.is_outside : false;
}

Unit Unit::fighter_parent() const
{
  return {impl->funcs.unit_is_fighter(u) ? u->fighter.parent : nullptr, impl};
}

Unit Unit::connectedUnit() const
{
  return {u->connected_unit, impl};
}

int Unit::hitPoints() const
{
  return (int)u->hp.raw_value;
}

int Unit::resourceCount() const
{
  return impl->funcs.ut_resource(u) ? u->building.resource.resource_count : 0;
}

u8 Unit::currentDirection1() const
{
  return (u8)u->heading.raw_value;
}

int Unit::current_speed_x() const
{
  return (int)u->velocity.x.raw_value;
}

int Unit::current_speed_y() const
{
  return (int)u->velocity.y.raw_value;
}

Unit Unit::subUnit() const
{
  return {u->subunit, impl};
}

int Unit::mainOrderTimer() const
{
  return u->main_order_timer;
}

int Unit::spellCooldown() const
{
  return u->spell_cooldown;
}

bool Unit::hasSprite_pImagePrimary() const
{
  return u->sprite->main_image;
}

int Unit::sprite_pImagePrimary_anim() const
{
  return u->sprite->main_image->iscript_state.animation;
}

Unit Unit::orderTarget_pUnit() const
{
  return {u->order_target.unit, impl};
}

int Unit::sprite_pImagePrimary_frameSet() const
{
  return u->sprite->main_image->frame_index_base;
}

int Unit::shieldPoints() const
{
  return (int)u->shield_points.raw_value;
}

int Unit::energy() const
{
  return (int)u->energy.raw_value;
}

int Unit::killCount() const
{
  return 0; // fix me
}

int Unit::acidSporeCount() const
{
  return u->acid_spore_count;
}

int Unit::defenseMatrixDamage() const
{
  return (int)u->defensive_matrix_hp.raw_value;
}

int Unit::defenseMatrixTimer() const
{
  return u->defensive_matrix_timer;
}

int Unit::ensnareTimer() const
{
  return u->ensnare_timer;
}

int Unit::irradiateTimer() const
{
  return u->irradiate_timer;
}

int Unit::lockdownTimer() const
{
  return u->lockdown_timer;
}

int Unit::maelstromTimer() const
{
  return u->maelstrom_timer;
}

int Unit::plagueTimer() const
{
  return u->plague_timer;
}

int Unit::removeTimer() const
{
  return u->remove_timer;
}

int Unit::stasisTimer() const
{
  return u->stasis_timer;
}

int Unit::stimTimer() const
{
  return u->stim_timer;
}

int Unit::secondaryOrderID() const
{
  return u->secondary_order_type ? (int)u->secondary_order_type->id : 0;
}

Unit Unit::currentBuildUnit() const
{
  return {u->current_build_unit, impl};
}

Unit Unit::moveTarget_pUnit() const
{
  return {u->move_target.unit, impl};
}

Position Unit::moveTarget() const
{
  return {(s16)u->move_target.pos.x, (s16)u->move_target.pos.y};
}

Position Unit::orderTarget() const
{
  return {(s16)u->order_target.pos.x, (s16)u->order_target.pos.y};
}

Unit Unit::building_addon() const
{
  return {u->building.addon, impl};
}

Unit Unit::nydus_exit() const
{
  return {impl->funcs.unit_is_nydus(u) ? u->building.nydus.exit : nullptr, impl};
}

Unit Unit::worker_pPowerup() const
{
  return {impl->funcs.ut_worker(u) ? u->worker.powerup : nullptr, impl};
}

int Unit::gatherQueueCount() const
{
  return impl->funcs.ut_resource(u) && u->building.resource.is_being_gathered ? 1 : 0;
}

Unit Unit::nextGatherer() const
{
  return {impl->funcs.ut_resource(u) && !u->building.resource.gather_queue.empty() ? &u->building.resource.gather_queue.front() : nullptr, impl};
}

int Unit::isBlind() const
{
  return u->blinded_by;
}


// Checked narrowing for MirrorFingerprint. A silent truncation here would make a CHANGED unit
// produce an UNCHANGED fingerprint — the mirror would then be skipped and go stale, which is the
// one failure this whole mechanism exists to prevent. So every narrowed field is range-checked and
// aborts loudly instead. The cost is one compare per field against a 216-byte memcpy; the compare
// is free next to the cache traffic the narrower struct saves.
namespace {
inline u8 fp_u8(int v, const char* f) {
  if ((unsigned)v > 0xffu) bwgame::error("MirrorFingerprint: %s = %d does not fit u8", f, v);
  return (u8)v;
}
inline u16 fp_u16(int v, const char* f) {
  if ((unsigned)v > 0xffffu) bwgame::error("MirrorFingerprint: %s = %d does not fit u16", f, v);
  return (u16)v;
}
inline s16 fp_s16(int v, const char* f) {
  if (v < -32768 || v > 32767) bwgame::error("MirrorFingerprint: %s = %d does not fit s16", f, v);
  return (s16)v;
}
inline s8 fp_s8(int v, const char* f) {
  if (v < -128 || v > 127) bwgame::error("MirrorFingerprint: %s = %d does not fit s8", f, v);
  return (s8)v;
}
}

void Unit::mirrorFingerprint(MirrorFingerprint* dst) const
{
  std::memset(dst, 0, sizeof(*dst));
  const bwgame::unit_t* p = u;
  dst->pos_x = fp_u16(p->position.x, "pos_x");
  dst->pos_y = fp_u16(p->position.y, "pos_y");
  const bwgame::sprite_t* s = p->sprite;
  dst->sprite = (u64)(uintptr_t)s;
  if (s)
  {
    dst->sprite_visibility = fp_u16((int)s->visibility_flags, "sprite_visibility");
    const bwgame::image_t* im = s->main_image;
    dst->main_image = (u64)(uintptr_t)im;
    if (im)
    {
      dst->image_anim = fp_u8((int)im->iscript_state.animation, "image_anim");
      dst->image_frameset = fp_u16((int)im->frame_index_base, "image_frameset");
    }
  }
  dst->unit_type = (u64)(uintptr_t)p->unit_type;
  dst->owner = fp_u8(p->owner, "owner");
  dst->order_type = (u64)(uintptr_t)p->order_type;
  dst->sec_order_type = (u64)(uintptr_t)p->secondary_order_type;
  dst->main_order_timer = fp_u8(p->main_order_timer, "main_order_timer");
  dst->ground_cd = fp_u8(p->ground_weapon_cooldown, "ground_cd");
  dst->air_cd = fp_u8(p->air_weapon_cooldown, "air_cd");
  dst->spell_cd = fp_u8(p->spell_cooldown, "spell_cd");
  dst->status_flags = p->status_flags;
  dst->carrying_flags = fp_u8(p->carrying_flags, "carrying_flags");
  dst->movement_state = fp_u8(p->movement_state, "movement_state");
  dst->movement_flags = fp_u8((int)p->movement_flags, "movement_flags");
  dst->detected_flags = p->detected_flags;
  dst->hp_raw = (s32)p->hp.raw_value;
  dst->shield_raw = (s32)p->shield_points.raw_value;
  dst->energy_raw = (s32)p->energy.raw_value;
  dst->heading = fp_s8((int)p->heading.raw_value, "heading");
  dst->vel_x = fp_s16((int)p->velocity.x.raw_value, "vel_x");
  dst->vel_y = fp_s16((int)p->velocity.y.raw_value, "vel_y");
  dst->mt_x = fp_u16(p->move_target.pos.x, "mt_x");
  dst->mt_y = fp_u16(p->move_target.pos.y, "mt_y");
  dst->mt_unit = (u64)(uintptr_t)p->move_target.unit;
  dst->ot_x = fp_u16(p->order_target.pos.x, "ot_x");
  dst->ot_y = fp_u16(p->order_target.pos.y, "ot_y");
  dst->ot_unit = (u64)(uintptr_t)p->order_target.unit;
  dst->subunit = (u64)(uintptr_t)p->subunit;
  dst->connected_unit = (u64)(uintptr_t)p->connected_unit;
  dst->current_build_unit = (u64)(uintptr_t)p->current_build_unit;
  dst->kill_count = p->kill_count;
  dst->acid_spore_count = fp_u8(p->acid_spore_count, "acid_spore_count");
  dst->parasite_flags = fp_u8(p->parasite_flags, "parasite_flags");
  dst->blinded_by = fp_u8(p->blinded_by, "blinded_by");
  dst->storm_timer = fp_u8(p->storm_timer, "storm_timer");
  dst->remove_timer = fp_u16(p->remove_timer, "remove_timer");
  dst->dm_timer = fp_u8(p->defensive_matrix_timer, "dm_timer");
  dst->dm_hp_raw = (s32)p->defensive_matrix_hp.raw_value;
  dst->stim_timer = fp_u8(p->stim_timer, "stim_timer");
  dst->ensnare_timer = fp_u8(p->ensnare_timer, "ensnare_timer");
  dst->lockdown_timer = fp_u8(p->lockdown_timer, "lockdown_timer");
  dst->irradiate_timer = fp_u8(p->irradiate_timer, "irradiate_timer");
  dst->stasis_timer = fp_u8(p->stasis_timer, "stasis_timer");
  dst->plague_timer = fp_u8(p->plague_timer, "plague_timer");
  dst->maelstrom_timer = fp_u8(p->maelstrom_timer, "maelstrom_timer");
  dst->remaining_build_time = fp_u16(p->remaining_build_time, "remaining_build_time");
  dst->build_queue_size = (u8)p->build_queue.size();
  for (size_t i = 0; i != p->build_queue.size() && i != 5; ++i)
    dst->build_queue[i] = (u64)(uintptr_t)p->build_queue[i];
  const bwgame::unit_t* su = p->subunit;
  if (su)
  {
    dst->sub_present = 1;
    dst->sub_ground_cd = fp_u8(su->ground_weapon_cooldown, "sub_ground_cd");
    dst->sub_air_cd = fp_u8(su->air_weapon_cooldown, "sub_air_cd");
    const bwgame::sprite_t* ss = su->sprite;
    dst->sub_sprite = (u64)(uintptr_t)ss;
    if (ss)
    {
      const bwgame::image_t* sim = ss->main_image;
      dst->sub_main_image = (u64)(uintptr_t)sim;
      if (sim)
      {
        dst->sub_anim = fp_u8((int)sim->iscript_state.animation, "sub_anim");
        dst->sub_frameset = fp_u16((int)sim->frame_index_base, "sub_frameset");
      }
    }
  }
  // Raw copies of the type-specific blocks: every union/worker/building-derived read
  // (resource counts, gather queue, addon/rally/silo/nydus links, fighter parent, mine
  // counts, hangar counts) is covered verbatim without per-type logic.
  constexpr size_t kUnion = sizeof(bwgame::unit_t{}.vulture) > sizeof(bwgame::unit_t{}.carrier)
                              ? sizeof(bwgame::unit_t{}.vulture) : sizeof(bwgame::unit_t{}.carrier);
  constexpr size_t kUnion2 = kUnion > sizeof(bwgame::unit_t{}.fighter) ? kUnion : sizeof(bwgame::unit_t{}.fighter);
  constexpr size_t kUnion3 = kUnion2 > sizeof(bwgame::unit_t{}.beacon) ? kUnion2 : sizeof(bwgame::unit_t{}.beacon);
  constexpr size_t kUnion4 = kUnion3 > sizeof(bwgame::unit_t{}.ghost) ? kUnion3 : sizeof(bwgame::unit_t{}.ghost);
  constexpr size_t kWorker = sizeof(bwgame::unit_t{}.worker);
  constexpr size_t kBuilding = sizeof(bwgame::unit_t{}.building);
  static_assert(kUnion4 + kWorker + kBuilding == sizeof(dst->raw_blocks),
                "MirrorFingerprint::raw_blocks must be EXACTLY the three unit_t blocks: too small "
                "loses coverage, too large compares guaranteed-zero padding every frame per unit "
                "per viewer (it was 320 for a 216-byte payload). Update kRawBlocks in BWData.h.");
  u8* b = dst->raw_blocks;
  std::memcpy(b, &p->vulture, kUnion4); b += kUnion4;
  std::memcpy(b, &p->worker, kWorker); b += kWorker;
  std::memcpy(b, &p->building, kBuilding);
}

int Unit::resourceType() const
{
  return u->carrying_flags;
}

int Unit::parasiteFlags() const
{
  return u->parasite_flags;
}

int Unit::stormTimer() const
{
  return u->storm_timer;
}

int Unit::movementState() const
{
  return u->movement_state;
}

int Unit::carrier_inHangerCount() const
{
  if (impl->funcs.unit_is_carrier(u)) return (int)u->carrier.inside_count;
  else if (impl->funcs.unit_is_reaver(u)) return (int)u->reaver.inside_count;
  else return 0;
}

int Unit::carrier_outHangerCount() const
{
  if (impl->funcs.unit_is_carrier(u)) return (int)u->carrier.outside_count;
  else if (impl->funcs.unit_is_reaver(u)) return (int)u->reaver.outside_count;
  else return 0;
}

int Unit::vulture_spiderMineCount() const
{
  return impl->funcs.unit_is_vulture(u) ? (int)u->vulture.spider_mine_count : 0;
}

int Unit::remainingBuildTime() const
{
  return u->remaining_build_time;
}

bool Unit::silo_bReady() const
{
  return impl->funcs.unit_is(u, bwgame::UnitTypes::Terran_Nuclear_Silo) ? u->building.silo.ready : false;
}

int Unit::building_larvaTimer() const
{
  return u->building.larva_timer;
}

int Unit::orderQueueTimer() const
{
  return u->order_process_timer;
}

int Unit::building_techType() const
{
  return u->building.researching_type ? (int)u->building.researching_type->id : 0;
}

int Unit::building_upgradeResearchTime() const
{
  return u->building.upgrade_research_time;
}

int Unit::building_upgradeType() const
{
  return u->building.upgrading_type ? (int)u->building.upgrading_type->id : 0;
}

Position Unit::rally_position() const
{
  return {(s16)u->building.rally.pos.x, (s16)u->building.rally.pos.y};
}

Unit Unit::rally_unit() const
{
  return {u->building.rally.unit, impl};
}

int Unit::orderState() const
{
  return u->order_state;
}

void Unit::setHitPoints(int value)
{
  bwgame::sync_functions::dynamic_writer<> w;
  w.template put<uint8_t>(210);
  w.template put<uint8_t>(0);
  w.template put<uint8_t>(0);
  w.template put<uint16_t>(impl->funcs.get_unit_id(u).raw_value);
  w.template put<int32_t>(value);
  impl->game_setup_helper.input_action(active_viewer, w.data(), w.size());
}

void Unit::setShields(int value)
{
  bwgame::sync_functions::dynamic_writer<> w;
  w.template put<uint8_t>(210);
  w.template put<uint8_t>(0);
  w.template put<uint8_t>(1);
  w.template put<uint16_t>(impl->funcs.get_unit_id(u).raw_value);
  w.template put<int32_t>(value);
  impl->game_setup_helper.input_action(active_viewer, w.data(), w.size());
}

void Unit::setEnergy(int value)
{
  bwgame::sync_functions::dynamic_writer<> w;
  w.template put<uint8_t>(210);
  w.template put<uint8_t>(0);
  w.template put<uint8_t>(2);
  w.template put<uint16_t>(impl->funcs.get_unit_id(u).raw_value);
  w.template put<int32_t>(value);
  impl->game_setup_helper.input_action(active_viewer, w.data(), w.size());
}


Bullet::operator bool() const
{
  return b != nullptr;
}

size_t Bullet::getIndex() const
{
  return b->index;
}

bool Bullet::hasSprite() const
{
  return b->sprite != nullptr;
}

Position Bullet::spritePosition() const
{
  return {(s16)b->sprite->position.x, (s16)b->sprite->position.y};
}

Position Bullet::position() const
{
  return {(s16)b->position.x, (s16)b->position.y};
}

Unit Bullet::sourceUnit() const
{
  return {b->bullet_owner_unit, impl};
}

Unit Bullet::attackTargetUnit() const
{
  return {b->bullet_target, impl};
}

int Bullet::type() const
{
  return (int)b->flingy_type->id;
}

int Bullet::currentDirection() const
{
  return (int)b->current_velocity_direction.raw_value;
}

int Bullet::current_speed_x() const
{
  return (int)b->velocity.x.raw_value;
}

int Bullet::current_speed_y() const
{
  return (int)b->velocity.y.raw_value;
}

Position Bullet::targetPosition() const
{
  return {(s16)b->next_movement_waypoint.x, (s16)b->next_movement_waypoint.y};
}

int Bullet::time_remaining() const
{
  return b->remaining_time;
}

UnitFinderIterator::value_type UnitFinderIterator::operator*() const
{
  return {ptr};
}

UnitFinderIterator& UnitFinderIterator::operator++()
{
  ptr = (void*)((bwgame::state_base_non_copyable::unit_finder_entry*)ptr + 1);
  return *this;
}

bool UnitFinderIterator::operator==(UnitFinderIterator::It n) const
{
  return ptr == n.ptr;
}

bool UnitFinderIterator::operator!=(UnitFinderIterator::It n) const
{
  return ptr != n.ptr;
}

UnitFinderIterator::pointer UnitFinderIterator::operator->() const
{
  return {ptr};
}

UnitFinderIterator::It UnitFinderIterator::operator++(int)
{
  auto r = *this;
  ++*this;
  return r;
}

UnitFinderIterator::It& UnitFinderIterator::operator--()
{
  ptr = (void*)((bwgame::state_base_non_copyable::unit_finder_entry*)ptr - 1);
  return *this;
}

UnitFinderIterator::It UnitFinderIterator::operator--(int)
{
  auto r = *this;
  --*this;
  return r;
}

UnitFinderIterator::It& UnitFinderIterator::operator+=(UnitFinderIterator::difference_type n)
{
  ptr = (void*)((bwgame::state_base_non_copyable::unit_finder_entry*)ptr + n);
  return *this;
}

UnitFinderIterator::It UnitFinderIterator::operator+(UnitFinderIterator::difference_type n) const
{
  return {(void*)((bwgame::state_base_non_copyable::unit_finder_entry*)ptr + n)};
}

UnitFinderIterator::It& UnitFinderIterator::operator-=(UnitFinderIterator::difference_type n)
{
  ptr = (void*)((bwgame::state_base_non_copyable::unit_finder_entry*)ptr - n);
  return *this;
}

UnitFinderIterator::It UnitFinderIterator::operator-(UnitFinderIterator::difference_type n) const
{
  return {(void*)((bwgame::state_base_non_copyable::unit_finder_entry*)ptr - n)};
}

UnitFinderIterator::difference_type UnitFinderIterator::operator-(UnitFinderIterator::It n) const
{
  return (bwgame::state_base_non_copyable::unit_finder_entry*)ptr - (bwgame::state_base_non_copyable::unit_finder_entry*)n.ptr;
}

UnitFinderIterator::value_type UnitFinderIterator::operator[](UnitFinderIterator::difference_type n) const
{
  return {(void*)((bwgame::state_base_non_copyable::unit_finder_entry*)ptr + n)};
}

bool UnitFinderIterator::operator<(UnitFinderIterator::It n) const
{
  return (bwgame::state_base_non_copyable::unit_finder_entry*)ptr < (bwgame::state_base_non_copyable::unit_finder_entry*)n.ptr;
}

bool UnitFinderIterator::operator>(UnitFinderIterator::It n) const
{
  return (bwgame::state_base_non_copyable::unit_finder_entry*)ptr > (bwgame::state_base_non_copyable::unit_finder_entry*)n.ptr;
}

bool UnitFinderIterator::operator<=(UnitFinderIterator::It n) const
{
  return (bwgame::state_base_non_copyable::unit_finder_entry*)ptr <= (bwgame::state_base_non_copyable::unit_finder_entry*)n.ptr;
}

bool UnitFinderIterator::operator>=(UnitFinderIterator::It n) const
{
  return (bwgame::state_base_non_copyable::unit_finder_entry*)ptr >= (bwgame::state_base_non_copyable::unit_finder_entry*)n.ptr;
}

int UnitFinderEntry::unitIndex() const
{
  return ((bwgame::state_base_non_copyable::unit_finder_entry*)ptr)->u->index + 1;
}

int UnitFinderEntry::searchValue() const
{
  return ((bwgame::state_base_non_copyable::unit_finder_entry*)ptr)->value;
}

Region::operator bool() const
{
  return index != (size_t)-1;
}

size_t Region::getIndex() const
{
  return index;
}

size_t Region::groupIndex() const
{
  return impl->funcs.game_st.regions.regions.at(index).group_index;
}

Position Region::getCenter() const
{
  bwgame::xy pos = impl->funcs.to_xy(impl->funcs.game_st.regions.regions.at(index).center);
  return {(s16)pos.x, (s16)pos.y};
}

int Region::accessabilityFlags() const
{
  return impl->funcs.game_st.regions.regions.at(index).flags;
}

int Region::rgnBox_left() const
{
  return impl->funcs.game_st.regions.regions.at(index).area.from.x;
}

int Region::rgnBox_right() const
{
  return impl->funcs.game_st.regions.regions.at(index).area.to.x;
}

int Region::rgnBox_top() const
{
  return impl->funcs.game_st.regions.regions.at(index).area.from.y;
}

int Region::rgnBox_bottom() const
{
  return impl->funcs.game_st.regions.regions.at(index).area.to.y;
}

size_t Region::neighborCount() const
{
  auto& r = impl->funcs.game_st.regions.regions.at(index);
  return r.walkable_neighbors.size() + r.non_walkable_neighbors.size();
}

Region Region::getNeighbor(size_t index) const
{
  auto& r = impl->funcs.game_st.regions.regions.at(index);
  if (index < r.walkable_neighbors.size()) return {r.walkable_neighbors[index]->index, impl};
  index -= r.walkable_neighbors.size();
  if (index < r.non_walkable_neighbors.size()) return {r.non_walkable_neighbors[index]->index, impl};
  return {};
}

template<typename T>
DefaultIterator<T>::DefaultIterator()
{
  obj.construct<itobj<T>>();
}

template<typename T>
DefaultIterator<T>::~DefaultIterator()
{
  obj.destroy<itobj<T>>();
}

template<typename T>
typename DefaultIterator<T>::value_type DefaultIterator<T>::operator*() const
{
  return {&*obj.as<itobj<T>>(), impl};
}

template<typename T>
typename DefaultIterator<T>::It& DefaultIterator<T>::operator++()
{
  ++obj.as<itobj<T>>();
  return *this;
}

template<typename T>
bool DefaultIterator<T>::operator==(const DefaultIterator<T>& n) const
{
  return obj.as<itobj<T>>() == n.obj.template as<itobj<T>>();
}

template<typename T>
bool DefaultIterator<T>::operator!=(const DefaultIterator<T>& n) const
{
  return obj.as<itobj<T>>() != n.obj.template as<itobj<T>>();
}

template<typename T>
typename DefaultIterator<T>::pointer DefaultIterator<T>::operator->() const
{
  return {**this};
}

template<typename T>
typename DefaultIterator<T>::It DefaultIterator<T>::operator++(int)
{
  auto r = *this;
  ++*this;
  return r;
}

template struct DefaultIterator<Unit>;
template struct DefaultIterator<Bullet>;



}
