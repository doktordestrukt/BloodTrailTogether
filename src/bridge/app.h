#pragma once
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

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

  // Character customisation (window <-> game script). Guarded by `mu`.
  std::vector<std::string> wardrobe;   // "CAT ..." lines from the script
  std::string look;                    // current look string (script loads it; the window edits it)
  std::atomic<bool> char_open{false};  // character menu open (the script renders the preview)
  std::atomic<float> char_yaw{0.0f};   // preview model rotation (degrees)
  std::atomic<bool> third_person{false};  // over-the-right-shoulder camera in game
};

App& app();

// Builds the reply for one request frame coming from the Lua side.
std::string lua_handle_request(const std::string& req);

void pipe_server_start();
void control_window_start();
void control_window_toggle();
void control_window_host();        // watch menu: host a listed game
void control_window_quick_join();  // watch menu: join the first compatible listed game

}  // namespace btt
