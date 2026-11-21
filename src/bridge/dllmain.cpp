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
