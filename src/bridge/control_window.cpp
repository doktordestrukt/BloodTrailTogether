// "BloodTrail Together" multiplayer panel: a small always-available desktop window
// (Create / Join / Settings / Disconnect, join code, connection status, update notice).
// Typing a join code is far easier on the desktop keyboard than in VR; the in-headset side
// shows status and notifications (see main.lua). Toggle with F9 in-game.
#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>

#include <cstdio>
#include <string>

#include "bridge/app.h"
#include "core/log.h"
#include "core/util.h"

namespace btt {

namespace {

enum Id {
  ID_STATUS = 100, ID_NAME, ID_CREATE, ID_CODE_OUT, ID_COPY, ID_CODE_IN, ID_JOIN, ID_DISCONNECT, ID_SHARE,
  ID_SIM_MIRROR, ID_SIM_ORBIT, ID_UPDATE, ID_DEV_LAT, ID_DEV_JIT, ID_DEV_LOSS, ID_DEV_APPLY, ID_DEV_UDPHOST,
  ID_DEV_UDPJOIN, ID_TIMER = 1
};

HWND g_wnd = nullptr;
HFONT g_font = nullptr, g_big = nullptr, g_title = nullptr;
HBRUSH g_bg = nullptr, g_edit_bg = nullptr;
const COLORREF kBg = RGB(18, 16, 16), kText = RGB(225, 220, 215), kAccent = RGB(190, 30, 30), kDim = RGB(140, 130, 125);
std::string g_last_code;

HWND add(HWND p, const wchar_t* cls, const wchar_t* text, DWORD style, int x, int y, int w, int h, int id, HFONT f = nullptr) {
  HWND c = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, x, y, w, h, p, (HMENU)(INT_PTR)id, nullptr, nullptr);
  SendMessageW(c, WM_SETFONT, (WPARAM)(f ? f : g_font), TRUE);
  return c;
}

std::string get_text(int id) {
  wchar_t buf[256];
  GetDlgItemTextW(g_wnd, id, buf, 256);
  return to_utf8(buf);
}

void set_text(int id, const std::string& s) {
  std::wstring w = to_wide(s);
  wchar_t cur[1024];
  GetDlgItemTextW(g_wnd, id, cur, 1024);
  if (w != cur) SetDlgItemTextW(g_wnd, id, w.c_str());
}

bool dev_ui() {
#ifdef BTT_RELEASE
  return false;
#else
  return app().settings.dev_mode;
#endif
}

void apply_name() {
  std::string n = get_text(ID_NAME);
  if (n.empty()) n = "Player";
  app().engine->set_player_name(n);
  app().save_name(n);
}

void copy_to_clipboard(const std::string& s) {
  if (!OpenClipboard(g_wnd)) return;
  EmptyClipboard();
  std::wstring w = to_wide(s);
  HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, (w.size() + 1) * sizeof(wchar_t));
  if (h) {
    memcpy(GlobalLock(h), w.c_str(), (w.size() + 1) * sizeof(wchar_t));
    GlobalUnlock(h);
    SetClipboardData(CF_UNICODETEXT, h);
  }
  CloseClipboard();
}

void refresh() {
  App& a = app();
  EngineSnapshot s = a.engine->snapshot();
  std::string st;
  static const char* q[] = {"", "poor", "ok", "good"};
  if (s.state == "Idle") st = "Not in a session.";
  else if (s.state == "Preparing") st = "Contacting Unity Relay...";
  else if (s.state == "Waiting") st = "Session open. Waiting for the other player to join...";
  else if (s.state == "Connecting") st = "Connecting...";
  else if (s.state == "Connected") {
    char b[256];
    snprintf(b, sizeof(b), "Connected to %s%s  |  ping %.0f ms  |  loss %.1f%%  |  %s", s.peer_name.c_str(),
             s.simulated ? " (simulated)" : "", s.rtt_ms, s.loss_pct, q[s.quality]);
    st = b;
  } else if (s.state == "Interrupted") st = "Connection interrupted - trying to recover...";
  else st = "Disconnected.";
  if (!s.error.empty() && (s.state == "Disconnected" || s.state == "Idle")) st += "\r\n" + s.error;
  if (s.state == "Connected" || s.state == "Interrupted") {
    st += "\r\nYou: " + (s.local_map.empty() ? std::string("?") : s.local_map) + "   Them: " +
          (s.remote_map.empty() ? std::string("?") : s.remote_map) + (s.same_map ? "   (same map)" : "   (different maps)");
  }
  if (!a.lua_connected) st += "\r\n[waiting for the in-game Lua mod]";
  set_text(ID_STATUS, st);

  std::string code = s.join_code.empty() ? "------" : s.join_code;
  if (code != g_last_code) {
    g_last_code = code;
    set_text(ID_CODE_OUT, code);
  }
  bool idle = s.state == "Idle" || s.state == "Disconnected";
  EnableWindow(GetDlgItem(g_wnd, ID_CREATE), idle);
  EnableWindow(GetDlgItem(g_wnd, ID_JOIN), idle);
  EnableWindow(GetDlgItem(g_wnd, ID_DISCONNECT), !idle);
  EnableWindow(GetDlgItem(g_wnd, ID_COPY), !s.join_code.empty());

  std::lock_guard<std::mutex> lk(a.mu);
  if (a.update.available) {
    set_text(ID_UPDATE, "Update available: " + a.update.latest_version + " - click to open the download page");
    ShowWindow(GetDlgItem(g_wnd, ID_UPDATE), SW_SHOW);
  }
}

LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM w, LPARAM l) {
  switch (m) {
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN: {
      HDC dc = (HDC)w;
      int id = GetDlgCtrlID((HWND)l);
      SetTextColor(dc, id == ID_CODE_OUT ? kAccent : id == ID_UPDATE ? RGB(240, 200, 80) : kText);
      SetBkColor(dc, kBg);
      return (LRESULT)g_bg;
    }
    case WM_CTLCOLOREDIT:
      SetTextColor((HDC)w, kText);
      SetBkColor((HDC)w, RGB(36, 32, 32));
      return (LRESULT)g_edit_bg;
    case WM_ERASEBKGND: {
      RECT r;
      GetClientRect(h, &r);
      FillRect((HDC)w, &r, g_bg);
      return 1;
    }
    case WM_TIMER:
      refresh();
      return 0;
    case WM_COMMAND: {
      NetEngine* e = app().engine.get();
      switch (LOWORD(w)) {
        case ID_CREATE:
          apply_name();
          e->host_relay();
          break;
        case ID_JOIN: {
          std::string code = get_text(ID_CODE_IN);
          if (code.empty()) {
            MessageBoxW(h, L"Enter the join code shown on the host's screen.", L"BloodTrail Together", MB_OK | MB_ICONINFORMATION);
            break;
          }
          apply_name();
          e->join_relay(code);
          break;
        }
        case ID_DISCONNECT: e->disconnect(); break;
        case ID_COPY: copy_to_clipboard(g_last_code); break;
        case ID_SHARE: e->set_share_sounds(IsDlgButtonChecked(h, ID_SHARE) == BST_CHECKED); break;
        case ID_SIM_MIRROR: apply_name(); e->start_simulated_peer(SimMode::Mirror); break;
        case ID_SIM_ORBIT: apply_name(); e->start_simulated_peer(SimMode::Orbit); break;
        case ID_UPDATE: {
          std::string url;
          {
            std::lock_guard<std::mutex> lk(app().mu);
            url = app().update.url;
          }
          if (url.rfind("https://github.com/", 0) == 0)
            ShellExecuteW(nullptr, L"open", to_wide(url).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
          break;
        }
        case ID_DEV_APPLY: {
          ImpairmentParams p;
          p.latency_ms = atoi(get_text(ID_DEV_LAT).c_str());
          p.jitter_ms = atoi(get_text(ID_DEV_JIT).c_str());
          p.loss_pct = atof(get_text(ID_DEV_LOSS).c_str());
          e->set_impairment(p);
          BTT_INFO("dev: impairment latency=%d jitter=%d loss=%.1f", p.latency_ms, p.jitter_ms, p.loss_pct);
          break;
        }
        case ID_DEV_UDPHOST: apply_name(); e->host_dev_udp(27777); break;
        case ID_DEV_UDPJOIN: apply_name(); e->join_dev_udp("127.0.0.1", 27777); break;
      }
      return 0;
    }
    case WM_CLOSE:  // closing just hides; the game keeps the mod running
      ShowWindow(h, SW_HIDE);
      app().window_visible = false;
      return 0;
    case WM_APP + 1:
      app().window_visible = !app().window_visible;
      ShowWindow(h, app().window_visible ? SW_SHOWNOACTIVATE : SW_HIDE);
      return 0;
  }
  return DefWindowProcW(h, m, w, l);
}

DWORD WINAPI ui_thread(LPVOID) {
  INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_STANDARD_CLASSES};
  InitCommonControlsEx(&icc);
  g_font = CreateFontW(-15, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
  g_big = CreateFontW(-34, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Consolas");
  g_title = CreateFontW(-22, 0, 0, 0, FW_HEAVY, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
  g_bg = CreateSolidBrush(kBg);
  g_edit_bg = CreateSolidBrush(RGB(36, 32, 32));

  WNDCLASSW wc{};
  wc.lpfnWndProc = wndproc;
  wc.hInstance = GetModuleHandleW(nullptr);
  wc.lpszClassName = L"BloodTrailTogetherPanel";
  wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  wc.hbrBackground = g_bg;
  RegisterClassW(&wc);

  bool dev = dev_ui();
  int H = dev ? 560 : 470;
  g_wnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, wc.lpszClassName, L"BloodTrail Together",
                          WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, 40, 40, 520, H, nullptr, nullptr,
                          wc.hInstance, nullptr);
  HWND w = g_wnd;
  add(w, L"STATIC", L"BLOODTRAIL TOGETHER", SS_LEFT, 16, 10, 300, 28, 0, g_title);
  add(w, L"STATIC", to_wide(std::string("v") + BTT_VERSION + "   F9 shows/hides this window").c_str(), SS_LEFT, 16, 38,
      480, 20, 0);
  add(w, L"STATIC", L"", SS_LEFT, 16, 64, 480, 66, ID_STATUS);

  add(w, L"STATIC", L"Your name:", SS_LEFT, 16, 140, 90, 22, 0);
  HWND name = add(w, L"EDIT", to_wide(app().settings.player_name).c_str(), WS_BORDER | ES_AUTOHSCROLL, 110, 138, 200, 24, ID_NAME);
  SendMessageW(name, EM_SETLIMITTEXT, 24, 0);

  add(w, L"BUTTON", L"CREATE SESSION", BS_PUSHBUTTON, 16, 176, 180, 34, ID_CREATE);
  add(w, L"STATIC", L"------", SS_CENTER, 210, 174, 190, 40, ID_CODE_OUT, g_big);
  add(w, L"BUTTON", L"Copy", BS_PUSHBUTTON, 410, 180, 80, 28, ID_COPY);

  add(w, L"STATIC", L"Join code:", SS_LEFT, 16, 230, 90, 22, 0);
  HWND code = add(w, L"EDIT", L"", WS_BORDER | ES_UPPERCASE | ES_AUTOHSCROLL, 110, 226, 180, 30, ID_CODE_IN, g_big);
  SendMessageW(code, EM_SETLIMITTEXT, 12, 0);
  add(w, L"BUTTON", L"JOIN SESSION", BS_PUSHBUTTON, 300, 226, 190, 32, ID_JOIN);

  add(w, L"BUTTON", L"DISCONNECT", BS_PUSHBUTTON, 16, 274, 180, 32, ID_DISCONNECT);
  HWND share = add(w, L"BUTTON", L"", BS_AUTOCHECKBOX, 210, 281, 18, 18, ID_SHARE);
  SendMessageW(share, BM_SETCHECK, BST_CHECKED, 0);
  add(w, L"STATIC", L"Share my gun / hand sounds", SS_LEFT, 232, 280, 260, 22, 0);

  add(w, L"STATIC", L"No second player around? Test with a simulated one:", SS_LEFT, 16, 324, 480, 20, 0);
  add(w, L"BUTTON", L"Simulated player: mirror", BS_PUSHBUTTON, 16, 348, 230, 30, ID_SIM_MIRROR);
  add(w, L"BUTTON", L"Simulated player: walk around", BS_PUSHBUTTON, 260, 348, 230, 30, ID_SIM_ORBIT);

  add(w, L"BUTTON", L"", BS_FLAT | BS_LEFT, 16, 392, 474, 26, ID_UPDATE);
  ShowWindow(GetDlgItem(w, ID_UPDATE), SW_HIDE);

  if (dev) {
    auto imp = app().engine->impairment();
    add(w, L"STATIC", L"DEV  latency ms / jitter ms / loss %:", SS_LEFT, 16, 430, 260, 22, 0);
    add(w, L"EDIT", std::to_wstring(imp.latency_ms).c_str(), WS_BORDER, 270, 428, 50, 24, ID_DEV_LAT);
    add(w, L"EDIT", std::to_wstring(imp.jitter_ms).c_str(), WS_BORDER, 326, 428, 50, 24, ID_DEV_JIT);
    add(w, L"EDIT", std::to_wstring((int)imp.loss_pct).c_str(), WS_BORDER, 382, 428, 50, 24, ID_DEV_LOSS);
    add(w, L"BUTTON", L"Apply", BS_PUSHBUTTON, 438, 428, 52, 24, ID_DEV_APPLY);
    add(w, L"BUTTON", L"DEV: UDP host :27777", BS_PUSHBUTTON, 16, 464, 230, 28, ID_DEV_UDPHOST);
    add(w, L"BUTTON", L"DEV: UDP join 127.0.0.1", BS_PUSHBUTTON, 260, 464, 230, 28, ID_DEV_UDPJOIN);
  }

  ShowWindow(w, SW_SHOWNOACTIVATE);
  SetTimer(w, ID_TIMER, 250, nullptr);
  refresh();
  MSG msg;
  while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
    if (!IsDialogMessageW(w, &msg)) {
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
    }
  }
  return 0;
}

}  // namespace

void control_window_start() {
  HANDLE t = CreateThread(nullptr, 0, ui_thread, nullptr, 0, nullptr);
  if (t) CloseHandle(t);
}

void control_window_toggle() {
  if (g_wnd) PostMessageW(g_wnd, WM_APP + 1, 0, 0);
}

}  // namespace btt
