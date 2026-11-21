#pragma once
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>

#include "core/config.h"
#include "engine/net_engine.h"
#include "engine/update_check.h"

namespace btt {

// Process-wide state of the in-game DLL.
struct App {
  std::wstring mod_dir;      // ...\ue4ss\Mods\BloodTrailTogether
  std::wstring config_path;  // mod_dir\config.ini
  Config config;
  Settings settings;
  std::unique_ptr<NetEngine> engine;

  std::mutex mu;
  UpdateInfo update;
  bool window_visible = true;
  std::string lua_version;
  bool lua_connected = false;
  int pipe_index = 0;

  // Bridge counters (diagnostics)
  std::atomic<uint64_t> stat_frames{0}, stat_poses{0}, stat_hands{0}, stat_remote{0}, stat_sounds_out{0},
      stat_sounds_in{0}, stat_travel{0};

  void save_name(const std::string& name);
};

App& app();

// Builds the reply for one request frame coming from the Lua side.
std::string lua_handle_request(const std::string& req);

void pipe_server_start();
void control_window_start();
void control_window_toggle();

}  // namespace btt
