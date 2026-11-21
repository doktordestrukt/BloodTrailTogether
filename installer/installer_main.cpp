// BloodTrail Together installer (GUI + command line).
//   GUI:  double-click. Detects the Steam install, Install / Repair / Uninstall / Launch.
//   CLI:  --detect | --install <gameDir> | --uninstall <gameDir> [--remove-ue4ss] | --info
//         [--payload <file>] (tests)   Exit code 0 = success.
#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shobjidl.h>

#include <cstdarg>
#include <cstdio>
#include <string>
#include <thread>

#include "install_core.h"

using namespace bttinst;

namespace {

Payload g_payload;
std::string g_payload_err;
FILE* g_log = nullptr;

void logline(const std::string& s) {
  if (!g_log) {
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    _wfopen_s(&g_log, (std::wstring(tmp) + L"BloodTrailTogether-installer.log").c_str(), L"a");
  }
  if (g_log) {
    fprintf(g_log, "%s\n", s.c_str());
    fflush(g_log);
  }
}

// Console output that is also appended to %TEMP%\BloodTrailTogether-installer.log
// (a GUI-subsystem exe has no stdout when launched without a console).
void say(const char* fmt, ...) {
  char buf[4096];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  fputs(buf, stdout);
  fflush(stdout);
  std::string s(buf);
  while (!s.empty() && s.back() == '\n') s.pop_back();
  logline(s);
}

bool supported_build(const std::string& build) {
  auto it = g_payload.meta.find("supported_builds");
  if (it == g_payload.meta.end() || build.empty()) return false;
  std::string list = "," + it->second + ",";
  return list.find("," + build + ",") != std::string::npos;
}

// ------------------------------------------------------------------ CLI
int run_cli(int argc, wchar_t** argv) {
  AttachConsole(ATTACH_PARENT_PROCESS);
  FILE* f;
  freopen_s(&f, "CONOUT$", "w", stdout);
  std::wstring cmd = argc > 1 ? argv[1] : L"";
  std::wstring dir = argc > 2 ? argv[2] : L"";
  bool remove_ue4ss = false;
  for (int i = 1; i < argc; ++i) {
    if (wcscmp(argv[i], L"--remove-ue4ss") == 0) remove_ue4ss = true;
    if (wcscmp(argv[i], L"--payload") == 0 && i + 1 < argc) {
      g_payload_err.clear();
      if (!load_payload_file(argv[i + 1], g_payload, g_payload_err)) {
        say("payload error: %s\n", g_payload_err.c_str());
        return 3;
      }
    }
  }
  auto prog = [](int pct, const std::string& line) {
    say("[%3d%%] %s\n", pct, line.c_str());
    logline(line);
  };
  if (cmd == L"--info") {
    say("payload: %s\n", g_payload_err.empty() ? "ok" : g_payload_err.c_str());
    for (auto& [k, v] : g_payload.meta) say("  %s = %s\n", k.c_str(), v.c_str());
    say("  files: %zu (UE4SS included: %s)\n", g_payload.files.size(), g_payload.has_ue4ss() ? "yes" : "no");
    return g_payload_err.empty() ? 0 : 3;
  }
  if (cmd == L"--detect") {
    for (auto& l : steam_libraries()) say("steam library: %s\n", narrow(l).c_str());
    auto games = find_game();
    for (auto& g : games) {
      auto ue = detect_ue4ss(g.dir, &g_payload);
      auto st = read_install_status(g.dir);
      say("game: %s\n  build %s (%s)\n  UE4SS: %s %s via %s\n  BloodTrail Together: %s %s\n", narrow(g.dir).c_str(),
             g.build_id.c_str(), supported_build(g.build_id) ? "supported" : "UNTESTED", ue.present ? "present" : "missing",
             ue.version.c_str(), narrow(ue.proxy).c_str(), st.installed ? "installed" : "not installed", st.version.c_str());
    }
    if (games.empty()) say("BloodTrail not found in any Steam library\n");
    return games.empty() ? 1 : 0;
  }
  if (cmd == L"--install") {
    if (!g_payload_err.empty()) { say("%s\n", g_payload_err.c_str()); return 3; }
    std::string b = read_build_id_for(dir);
    say("game build: %s (%s)\n", b.c_str(), supported_build(b) ? "supported" : "untested");
    Result r = install(dir, g_payload, prog);
    say("%s\n", r.message.c_str());
    return r.ok ? 0 : 1;
  }
  if (cmd == L"--uninstall") {
    Result r = uninstall(dir, remove_ue4ss, prog);
    say("%s\n", r.message.c_str());
    return r.ok ? 0 : 1;
  }
  say("usage: installer --detect | --install <dir> | --uninstall <dir> [--remove-ue4ss] | --info [--payload file]\n");
  return 2;
}

// ------------------------------------------------------------------ GUI
enum { ID_PATH = 200, ID_BROWSE, ID_INFO, ID_INSTALL, ID_UNINSTALL, ID_LAUNCH, ID_CLOSE, ID_PROGRESS, ID_LOG, ID_REMOVE_UE4SS };
constexpr UINT WM_PROGRESS = WM_APP + 10, WM_DONE = WM_APP + 11;

HWND g_wnd;
HFONT g_font, g_title;
HBRUSH g_bg, g_edit;
const COLORREF kBg = RGB(18, 16, 16), kText = RGB(225, 220, 215);
bool g_busy = false;

HWND add(const wchar_t* cls, const wchar_t* text, DWORD style, int x, int y, int w, int h, int id, HFONT f = nullptr) {
  HWND c = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, x, y, w, h, g_wnd, (HMENU)(INT_PTR)id, nullptr, nullptr);
  SendMessageW(c, WM_SETFONT, (WPARAM)(f ? f : g_font), TRUE);
  return c;
}

std::wstring path_text() {
  wchar_t buf[1024];
  GetDlgItemTextW(g_wnd, ID_PATH, buf, 1024);
  return buf;
}

void append_log(const std::string& s) {
  HWND e = GetDlgItem(g_wnd, ID_LOG);
  int n = GetWindowTextLengthW(e);
  SendMessageW(e, EM_SETSEL, n, n);
  std::wstring w = widen(s) + L"\r\n";
  SendMessageW(e, EM_REPLACESEL, FALSE, (LPARAM)w.c_str());
  logline(s);
}

void refresh_info() {
  std::wstring dir = path_text();
  std::string info;
  bool ok = looks_like_game_dir(dir);
  if (!ok) {
    info = "Select your BloodTrail folder (the one that contains BTVR.exe).";
  } else {
    std::string b = read_build_id_for(dir);
    auto ue = detect_ue4ss(dir, &g_payload);
    auto st = read_install_status(dir);
    info = "Game build: " + (b.empty() ? std::string("unknown") : b) + (supported_build(b) ? "  (supported)" : "  (not tested with this version)");
    info += "\r\nUE4SS: " + std::string(ue.present ? "installed" + (ue.version.empty() ? "" : " - " + ue.version)
                                                    : (g_payload.has_ue4ss() ? "not installed - will be installed" : "not installed"));
    info += "\r\nBloodTrail Together: " + std::string(st.installed ? "installed (" + st.version + ")" : "not installed") +
            "   |   this installer: " + g_payload.version();
    SetDlgItemTextW(g_wnd, ID_INSTALL, st.installed ? L"REPAIR / UPDATE" : L"INSTALL");
    EnableWindow(GetDlgItem(g_wnd, ID_UNINSTALL), st.installed && !g_busy);
  }
  SetDlgItemTextW(g_wnd, ID_INFO, widen(info).c_str());
  EnableWindow(GetDlgItem(g_wnd, ID_INSTALL), ok && !g_busy && g_payload_err.empty());
}

void browse() {
  IFileOpenDialog* dlg = nullptr;
  if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&dlg)))) return;
  DWORD opts;
  dlg->GetOptions(&opts);
  dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
  dlg->SetTitle(L"Select the BloodTrail folder (contains BTVR.exe)");
  if (SUCCEEDED(dlg->Show(g_wnd))) {
    IShellItem* item;
    if (SUCCEEDED(dlg->GetResult(&item))) {
      PWSTR p;
      if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &p))) {
        SetDlgItemTextW(g_wnd, ID_PATH, p);
        CoTaskMemFree(p);
      }
      item->Release();
    }
  }
  dlg->Release();
  refresh_info();
}

void run_job(bool do_install) {
  std::wstring dir = path_text();
  if (do_install) {
    std::string b = read_build_id_for(dir);
    if (!supported_build(b) &&
        MessageBoxW(g_wnd,
                    L"This BloodTrail version has not been tested with this release of BloodTrail Together.\n"
                    L"It will probably work, but some features may not.\n\nInstall anyway?",
                    L"BloodTrail Together", MB_YESNO | MB_ICONWARNING) != IDYES)
      return;
  } else if (MessageBoxW(g_wnd, L"Remove BloodTrail Together from this game?", L"BloodTrail Together",
                         MB_YESNO | MB_ICONQUESTION) != IDYES) {
    return;
  }
  bool remove_ue4ss = IsDlgButtonChecked(g_wnd, ID_REMOVE_UE4SS) == BST_CHECKED;
  g_busy = true;
  refresh_info();
  std::thread([dir, do_install, remove_ue4ss] {
    auto prog = [](int pct, const std::string& line) {
      PostMessageW(g_wnd, WM_PROGRESS, pct, (LPARAM) new std::string(line));
    };
    Result r = do_install ? install(dir, g_payload, prog) : uninstall(dir, remove_ue4ss, prog);
    PostMessageW(g_wnd, WM_DONE, r.ok, (LPARAM) new std::string(r.message));
  }).detach();
}

LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM w, LPARAM l) {
  switch (m) {
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN:
      SetTextColor((HDC)w, kText);
      SetBkColor((HDC)w, kBg);
      return (LRESULT)g_bg;
    case WM_CTLCOLOREDIT:
      SetTextColor((HDC)w, kText);
      SetBkColor((HDC)w, RGB(36, 32, 32));
      return (LRESULT)g_edit;
    case WM_ERASEBKGND: {
      RECT r;
      GetClientRect(h, &r);
      FillRect((HDC)w, &r, g_bg);
      return 1;
    }
    case WM_PROGRESS: {
      auto* s = (std::string*)l;
      SendDlgItemMessageW(h, ID_PROGRESS, PBM_SETPOS, w, 0);
      append_log(*s);
      delete s;
      return 0;
    }
    case WM_DONE: {
      auto* s = (std::string*)l;
      g_busy = false;
      append_log(*s);
      refresh_info();
      MessageBoxW(h, widen(*s).c_str(), L"BloodTrail Together", MB_OK | (w ? MB_ICONINFORMATION : MB_ICONERROR));
      delete s;
      return 0;
    }
    case WM_COMMAND:
      switch (LOWORD(w)) {
        case ID_BROWSE: browse(); break;
        case ID_PATH:
          if (HIWORD(w) == EN_CHANGE) refresh_info();
          break;
        case ID_INSTALL: run_job(true); break;
        case ID_UNINSTALL: run_job(false); break;
        case ID_LAUNCH: ShellExecuteW(nullptr, L"open", L"steam://rungameid/1032430", nullptr, nullptr, SW_SHOWNORMAL); break;
        case ID_CLOSE: if (!g_busy) DestroyWindow(h); break;
      }
      return 0;
    case WM_CLOSE:
      if (!g_busy) DestroyWindow(h);
      return 0;
    case WM_DESTROY:
      PostQuitMessage(0);
      return 0;
  }
  return DefWindowProcW(h, m, w, l);
}

int run_gui() {
  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_STANDARD_CLASSES | ICC_PROGRESS_CLASS};
  InitCommonControlsEx(&icc);
  g_font = CreateFontW(-15, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
  g_title = CreateFontW(-26, 0, 0, 0, FW_HEAVY, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
  g_bg = CreateSolidBrush(kBg);
  g_edit = CreateSolidBrush(RGB(36, 32, 32));
  WNDCLASSW wc{};
  wc.lpfnWndProc = wndproc;
  wc.hInstance = GetModuleHandleW(nullptr);
  wc.lpszClassName = L"BTTInstaller";
  wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  wc.hIcon = LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1));
  RegisterClassW(&wc);
  g_wnd = CreateWindowExW(0, wc.lpszClassName, L"BloodTrail Together Setup", WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                          CW_USEDEFAULT, CW_USEDEFAULT, 640, 560, nullptr, nullptr, wc.hInstance, nullptr);
  add(L"STATIC", L"BLOODTRAIL TOGETHER", SS_LEFT, 20, 14, 600, 34, 0, g_title);
  add(L"STATIC", widen("Unofficial two-player PCVR multiplayer mod  -  version " + g_payload.version()).c_str(), SS_LEFT, 20, 50, 600, 22, 0);
  add(L"STATIC", L"BloodTrail folder:", SS_LEFT, 20, 86, 600, 20, 0);
  add(L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL, 20, 108, 480, 26, ID_PATH);
  add(L"BUTTON", L"Browse...", BS_PUSHBUTTON, 510, 107, 100, 28, ID_BROWSE);
  add(L"STATIC", L"", SS_LEFT, 20, 144, 590, 64, ID_INFO);
  add(L"BUTTON", L"INSTALL", BS_DEFPUSHBUTTON, 20, 216, 190, 40, ID_INSTALL);
  add(L"BUTTON", L"Uninstall", BS_PUSHBUTTON, 220, 216, 120, 40, ID_UNINSTALL);
  add(L"BUTTON", L"Launch BloodTrail", BS_PUSHBUTTON, 350, 216, 150, 40, ID_LAUNCH);
  add(L"BUTTON", L"Close", BS_PUSHBUTTON, 510, 216, 100, 40, ID_CLOSE);
  // Themed checkboxes ignore custom text colours, so the label is a separate static control.
  add(L"BUTTON", L"", BS_AUTOCHECKBOX, 20, 264, 18, 18, ID_REMOVE_UE4SS);
  add(L"STATIC", L"When uninstalling, also remove UE4SS if this installer added it and no other mods use it", SS_LEFT,
      42, 263, 570, 22, 0);
  HWND pb = add(PROGRESS_CLASSW, L"", 0, 20, 292, 590, 18, ID_PROGRESS);
  SendMessageW(pb, PBM_SETRANGE, 0, MAKELPARAM(0, 100));
  add(L"EDIT", L"", WS_BORDER | WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL, 20, 318, 590, 190, ID_LOG);

  if (!g_payload_err.empty()) append_log("ERROR: " + g_payload_err);
  auto games = find_game();
  if (!games.empty()) {
    SetDlgItemTextW(g_wnd, ID_PATH, games[0].dir.c_str());
    append_log("Found BloodTrail via Steam: " + narrow(games[0].dir));
  } else {
    append_log("BloodTrail was not found automatically. Click Browse and select the BloodTrail folder.");
  }
  append_log("BloodTrail Together never modifies the game executable or your Steam files.");
  refresh_info();
  ShowWindow(g_wnd, SW_SHOW);
  MSG msg;
  while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
    if (!IsDialogMessageW(g_wnd, &msg)) {
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
    }
  }
  return 0;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
  int argc = 0;
  wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
  load_payload_resource(g_payload, g_payload_err);
  if (argc > 1) return run_cli(argc, argv);
  return run_gui();
}
