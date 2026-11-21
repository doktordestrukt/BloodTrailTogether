// btt_core.dll: loaded into the BloodTrail process by the UE4SS Lua mod via
// package.loadlib(path, "*") (which only calls LoadLibrary). Everything starts on a worker
// thread created from DllMain; DllMain itself does no work under the loader lock.
#include <windows.h>

#include <thread>

#include "bridge/app.h"
#include "core/log.h"
#include "core/util.h"

namespace btt {

static App* g_app = nullptr;
App& app() { return *g_app; }

void App::save_name(const std::string& name) {
  std::lock_guard<std::mutex> lk(mu);
  config.set("Player.Name", name);
  config.save(config_path);
}

static DWORD WINAPI init_thread(LPVOID) {
  App& a = app();
  // DLL lives in <mod>\bin\btt_core.dll
  std::wstring bin = module_dir();
  auto p = bin.find_last_of(L"\\/");
  a.mod_dir = p == std::wstring::npos ? bin : bin.substr(0, p);
  a.config_path = a.mod_dir + L"\\config.ini";
  a.config.load(a.config_path);
  a.config.load(a.mod_dir + L"\\dev.ini");  // optional developer overrides (never shipped)
  a.settings = Settings::from(a.config);
#ifdef BTT_RELEASE
  a.settings.dev_mode = false;
#endif
  log_init(a.mod_dir + L"\\BloodTrailTogether.log", false, a.settings.dev_mode ? LogLevel::Debug : LogLevel::Info);
  BTT_INFO("BloodTrail Together %s starting (pid %lu)%s", BTT_VERSION, GetCurrentProcessId(),
           a.settings.dev_mode ? " [developer mode]" : "");
  if (a.settings.unity_project_id.empty())
    BTT_WARN("Relay.ProjectId is empty: online sessions are unavailable (simulated player still works)");

  a.engine = std::make_unique<NetEngine>(a.settings);
  a.engine->start();
  pipe_server_start();
  control_window_start();

  if (a.settings.dev_mode) {
    // Developer automation for unattended tests: Dev.AutoStart = sim_mirror | sim_orbit | udp_host | udp_join
    std::string auto_start = a.config.get("Dev.AutoStart");
    std::thread([auto_start] {
      App& a = app();
      if (!auto_start.empty()) {
        while (!a.lua_connected) Sleep(200);  // wait until the game-side script is alive
        Sleep(2000);
        BTT_INFO("dev: auto-start %s", auto_start.c_str());
        if (auto_start == "sim_mirror") a.engine->start_simulated_peer(SimMode::Mirror);
        else if (auto_start == "sim_orbit") a.engine->start_simulated_peer(SimMode::Orbit);
        else if (auto_start == "udp_host") a.engine->host_dev_udp(27777);
        else if (auto_start == "udp_join") a.engine->join_dev_udp("127.0.0.1", 27777);
      }
      // Periodic diagnostics so a session can be judged from the log alone.
      for (;;) {
        Sleep(5000);
        EngineSnapshot s = a.engine->snapshot();
        BTT_INFO("dev stats: state=%s peer=%s rtt=%.0fms loss=%.1f%% map=%s remote_map=%s | lua frames=%llu poses=%llu "
                 "hands=%llu remote_served=%llu sounds_out=%llu sounds_in=%llu travel=%llu",
                 s.state.c_str(), s.peer_name.c_str(), s.rtt_ms, s.loss_pct, s.local_map.c_str(), s.remote_map.c_str(),
                 (unsigned long long)a.stat_frames.load(), (unsigned long long)a.stat_poses.load(),
                 (unsigned long long)a.stat_hands.load(), (unsigned long long)a.stat_remote.load(),
                 (unsigned long long)a.stat_sounds_out.load(), (unsigned long long)a.stat_sounds_in.load(),
                 (unsigned long long)a.stat_travel.load());
      }
    }).detach();
  }

  if (a.settings.update_check && !a.settings.update_repo.empty()) {
    std::thread([] {
      Sleep(5000);  // don't compete with game start-up
      UpdateInfo u = check_for_update(app().settings.update_repo, BTT_VERSION);
      std::lock_guard<std::mutex> lk(app().mu);
      app().update = u;
    }).detach();
  }
  return 0;
}

}  // namespace btt

BOOL APIENTRY DllMain(HMODULE mod, DWORD reason, LPVOID) {
  if (reason == DLL_PROCESS_ATTACH) {
    DisableThreadLibraryCalls(mod);
    // Pin the module: the Lua state may be reloaded (UE4SS hot reload) but threads keep running.
    HMODULE pinned;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                       reinterpret_cast<LPCWSTR>(&DllMain), &pinned);
    btt::g_app = new btt::App();
    HANDLE t = CreateThread(nullptr, 0, btt::init_thread, nullptr, 0, nullptr);
    if (t) CloseHandle(t);
  }
  return TRUE;
}
