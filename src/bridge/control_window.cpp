// "BloodTrail Together" multiplayer panel (F9 in game). Fully custom-drawn with GDI+ and animated
// at ~60 fps while visible: hover glows, pressed states, pulsing connection light, sliding server
// rows, fading toasts. Hosting puts your game in a public SERVER LIST (Unity Lobby); joining is a
// click on a row - no codes to type.
#include <windows.h>
#include <windowsx.h>
#include <objidl.h>
#include <gdiplus.h>
#include <shellapi.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "bridge/app.h"
#include "core/log.h"
#include "core/util.h"
#include "relay/lobby.h"

namespace btt {

namespace {

using namespace Gdiplus;

constexpr int W = 560, H = 640;
constexpr UINT_PTR TIMER_ANIM = 1;
constexpr int ID_NAME = 200;

enum Action { A_NONE, A_HOST, A_REFRESH, A_DISCONNECT, A_SHARE, A_SIM_MIRROR, A_SIM_ORBIT, A_UPDATE, A_DEV_HOST, A_DEV_JOIN, A_JOIN_ROW, A_UPDATE_ROW };

struct Hot {
  RectF r;
  Action act;
  int row = -1;
  bool enabled = true;
};

HWND g_wnd = nullptr, g_name = nullptr;
HFONT g_name_font = nullptr;
HBRUSH g_edit_bg = nullptr;
ULONG_PTR g_gdip = 0;
std::unique_ptr<LobbyClient> g_lobby;

// animation state
double g_t0 = 0;
float g_hover[16] = {};       // per-action hover amount 0..1
float g_row_hover[64] = {};   // per server row
float g_row_in[64] = {};      // per server row slide-in
float g_share_knob = 1;
float g_open = 0;             // window fade-in
int g_mouse_x = -1, g_mouse_y = -1;
bool g_mouse_down = false;
float g_scroll = 0, g_scroll_target = 0;
std::vector<Hot> g_hots;
std::vector<ServerEntry> g_servers;
std::string g_list_sig;
double g_last_auto_refresh = 0;
std::wstring g_toast;
double g_toast_until = 0;
bool g_share = true;

bool dev_ui() {
#ifdef BTT_RELEASE
  return false;
#else
  return app().settings.dev_mode;
#endif
}

std::string short_map(const std::string& p) {
  size_t s = p.find_last_of('/');
  std::string m = s == std::string::npos ? p : p.substr(s + 1);
  if (m.rfind("Map_", 0) == 0) m = m.substr(4);
  for (auto& c : m) if (c == '_') c = ' ';
  return m.empty() ? "?" : m;
}

std::string current_name() {
  wchar_t buf[64];
  GetWindowTextW(g_name, buf, 64);
  std::string n = to_utf8(buf);
  return n.empty() ? "Player" : n;
}

void apply_name() {
  std::string n = current_name();
  app().engine->set_player_name(n);
  app().save_name(n);
}

void toast(const std::wstring& s) {
  g_toast = s;
  g_toast_until = now_sec() + 3.0;
}

float approach(float v, float target, float speed) { return v + (target - v) * std::min(1.0f, speed); }

Color rgba(int r, int g, int b, int a = 255) { return Color((BYTE)std::clamp(a, 0, 255), (BYTE)r, (BYTE)g, (BYTE)b); }

void round_rect(GraphicsPath& p, RectF r, float rad) {
  float d = rad * 2;
  p.AddArc(r.X, r.Y, d, d, 180, 90);
  p.AddArc(r.X + r.Width - d, r.Y, d, d, 270, 90);
  p.AddArc(r.X + r.Width - d, r.Y + r.Height - d, d, d, 0, 90);
  p.AddArc(r.X, r.Y + r.Height - d, d, d, 90, 90);
  p.CloseFigure();
}

void fill_round(Graphics& g, RectF r, float rad, const Brush& b) {
  GraphicsPath p;
  round_rect(p, r, rad);
  g.FillPath(&b, &p);
}

void stroke_round(Graphics& g, RectF r, float rad, Color c, float w) {
  GraphicsPath p;
  round_rect(p, r, rad);
  Pen pen(c, w);
  g.DrawPath(&pen, &p);
}

void text(Graphics& g, const std::wstring& s, RectF r, float size, Color c, int style = FontStyleRegular,
          StringAlignment h = StringAlignmentNear, StringAlignment v = StringAlignmentCenter, const wchar_t* fam = L"Segoe UI") {
  FontFamily ff(fam);
  Font f(&ff, size, style, UnitPixel);
  StringFormat sf;
  sf.SetAlignment(h);
  sf.SetLineAlignment(v);
  sf.SetTrimming(StringTrimmingEllipsisCharacter);
  SolidBrush b(c);
  g.DrawString(s.c_str(), -1, &f, r, &sf, &b);
}

bool inside(const RectF& r, int x, int y) { return x >= r.X && x < r.X + r.Width && y >= r.Y && y < r.Y + r.Height; }

// A button: dark glass with a crimson glow that grows on hover.
void button(Graphics& g, RectF r, const std::wstring& label, Action act, bool enabled, bool primary, int row = -1) {
  g_hots.push_back({r, act, row, enabled});
  float hv = row >= 0 ? g_row_hover[row] : g_hover[act];
  bool pressed = enabled && g_mouse_down && inside(r, g_mouse_x, g_mouse_y);
  RectF rr = r;
  if (pressed) rr.Inflate(-1.5f, -1.5f);
  if (!enabled) {
    SolidBrush b(rgba(40, 36, 36));
    fill_round(g, rr, 8, b);
    text(g, label, rr, 14, rgba(110, 100, 98), FontStyleBold, StringAlignmentCenter);
    return;
  }
  // glow
  for (int i = 3; i >= 1; --i) {
    RectF gr = rr;
    gr.Inflate(i * 2.5f * hv, i * 2.5f * hv);
    SolidBrush gb(rgba(200, 20, 20, (int)(28 * hv)));
    fill_round(g, gr, 8 + i * 2.5f * hv, gb);
  }
  Color top = primary ? rgba(150 + (int)(60 * hv), 18, 18) : rgba(46 + (int)(20 * hv), 38, 38);
  Color bot = primary ? rgba(95 + (int)(40 * hv), 8, 8) : rgba(30 + (int)(12 * hv), 25, 25);
  LinearGradientBrush lb(PointF(rr.X, rr.Y), PointF(rr.X, rr.Y + rr.Height), top, bot);
  fill_round(g, rr, 8, lb);
  stroke_round(g, rr, 8, primary ? rgba(255, 90, 80, 90 + (int)(120 * hv)) : rgba(160, 60, 55, 50 + (int)(140 * hv)), 1.2f);
  text(g, label, rr, 14, rgba(245, 238, 232), FontStyleBold, StringAlignmentCenter);
}

void draw(HDC dc) {
  App& a = app();
  EngineSnapshot s = a.engine->snapshot();
  double t = now_sec() - g_t0;
  g_hots.clear();

  Bitmap bmp(W, H, PixelFormat32bppPARGB);
  Graphics g(&bmp);
  g.SetSmoothingMode(SmoothingModeAntiAlias);
  g.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);

  // background: deep red-black gradient with a slow drifting vignette
  LinearGradientBrush bg(PointF(0, 0), PointF(0, (REAL)H), rgba(26, 12, 12), rgba(9, 8, 8));
  g.FillRectangle(&bg, 0, 0, W, H);
  {
    float cx = (float)(W * 0.5 + 120 * std::sin(t * 0.25)), cy = 40.0f;
    GraphicsPath ep;
    ep.AddEllipse(cx - 320, cy - 220, 640.0f, 440.0f);
    PathGradientBrush pg(&ep);
    pg.SetCenterColor(rgba(120, 10, 10, 70));
    Color edge[] = {rgba(0, 0, 0, 0)};
    int n = 1;
    pg.SetSurroundColors(edge, &n);
    g.FillPath(&pg, &ep);
  }

  // header
  text(g, L"BLOODTRAIL", RectF(24, 14, 400, 34), 28, rgba(235, 228, 220), FontStyleBold, StringAlignmentNear, StringAlignmentCenter, L"Segoe UI Black");
  text(g, L"TOGETHER", RectF(232, 18, 300, 30), 22, rgba(205, 32, 32), FontStyleBold, StringAlignmentNear, StringAlignmentCenter, L"Segoe UI Black");
  text(g, to_wide(std::string("v") + BTT_VERSION) + L"  \x2022  F9 shows / hides", RectF(24, 46, 400, 18), 12, rgba(150, 135, 130));
  {  // animated accent line
    float sweep = (float)std::fmod(t * 0.35, 1.0);
    LinearGradientBrush lb(PointF(0, 0), PointF((REAL)W, 0), rgba(120, 10, 10, 0), rgba(120, 10, 10, 0));
    Color cols[] = {rgba(120, 10, 10, 0), rgba(220, 30, 30, 230), rgba(120, 10, 10, 0)};
    REAL pos[] = {0.0f, std::clamp(sweep, 0.01f, 0.99f), 1.0f};
    lb.SetInterpolationColors(cols, pos, 3);
    g.FillRectangle(&lb, 24.0f, 70.0f, (REAL)(W - 48), 2.0f);
  }

  // status card
  RectF card(24, 84, (REAL)(W - 48), 92);
  SolidBrush cb(rgba(255, 255, 255, 10));
  fill_round(g, card, 12, cb);
  stroke_round(g, card, 12, rgba(255, 255, 255, 22), 1);
  Color dot = rgba(110, 100, 98);
  std::wstring head = L"Not in a game", sub = L"Host a game or pick one from the server list.";
  if (s.state == "Preparing" || s.state == "Connecting") { dot = rgba(240, 190, 60); head = s.state == "Preparing" ? L"Setting up your game..." : L"Connecting..."; sub = L"Contacting Unity Relay"; }
  else if (s.state == "Waiting") {
    dot = rgba(240, 190, 60);
    head = L"Hosting \x2014 waiting for a player";
    sub = g_lobby && g_lobby->listed() ? L"Your game is in the server list." : L"Adding your game to the server list...";
  } else if (s.state == "Connected") {
    dot = s.quality >= 3 ? rgba(90, 220, 110) : s.quality == 2 ? rgba(240, 190, 60) : rgba(235, 70, 60);
    head = L"Playing with " + to_wide(s.peer_name) + (s.simulated ? L" (simulated)" : L"");
    wchar_t b[200];
    swprintf(b, 200, L"%s  \x2022  you: %s  \x2022  them: %s", s.same_map ? L"same map" : L"different maps",
             to_wide(short_map(s.local_map)).c_str(), to_wide(short_map(s.remote_map)).c_str());
    sub = b;
  } else if (s.state == "Interrupted") { dot = rgba(235, 70, 60); head = L"Connection interrupted"; sub = L"Trying to recover..."; }
  else if (s.state == "Disconnected") { head = L"Disconnected"; sub = to_wide(s.error.empty() ? std::string("The session ended.") : s.error); }
  if (!a.lua_connected) sub += L"   [waiting for the game]";
  float pulse = (float)(0.5 + 0.5 * std::sin(t * 4.0));
  SolidBrush halo(Color((BYTE)(60 + 80 * pulse), dot.GetR(), dot.GetG(), dot.GetB()));
  g.FillEllipse(&halo, card.X + 18 - 6 * pulse, card.Y + 24 - 6 * pulse, 14 + 12 * pulse, 14 + 12 * pulse);
  SolidBrush db(dot);
  g.FillEllipse(&db, card.X + 18.0f, card.Y + 24.0f, 14.0f, 14.0f);
  text(g, head, RectF(card.X + 44, card.Y + 12, card.Width - 150, 36), 18, rgba(240, 232, 226), FontStyleBold);
  text(g, sub, RectF(card.X + 44, card.Y + 46, card.Width - 60, 36), 12.5f, rgba(170, 158, 152), FontStyleRegular, StringAlignmentNear, StringAlignmentNear);
  if (s.state == "Connected") {  // ping meter: 5 bars
    int good = s.rtt_ms < 60 ? 5 : s.rtt_ms < 100 ? 4 : s.rtt_ms < 160 ? 3 : s.rtt_ms < 250 ? 2 : 1;
    for (int i = 0; i < 5; ++i) {
      SolidBrush pb(i < good ? dot : rgba(70, 62, 60));
      g.FillRectangle(&pb, card.X + card.Width - 96 + i * 9.0f, card.Y + 34 - i * 4.0f, 6.0f, 8.0f + i * 4.0f);
    }
    wchar_t pb[32];
    swprintf(pb, 32, L"%.0f ms", s.rtt_ms);
    text(g, pb, RectF(card.X + card.Width - 100, card.Y + 50, 90, 20), 12, rgba(200, 190, 185), FontStyleBold, StringAlignmentNear);
  }

  // name row (the edit control sits on top of the drawn field)
  text(g, L"YOUR NAME", RectF(24, 190, 120, 30), 11.5f, rgba(160, 146, 140), FontStyleBold);
  RectF nf(130, 188, 220, 34);
  SolidBrush nb(rgba(0, 0, 0, 90));
  fill_round(g, nf, 8, nb);
  stroke_round(g, nf, 8, rgba(200, 40, 40, GetFocus() == g_name ? 200 : 70), 1.2f);

  bool idle = s.state == "Idle" || s.state == "Disconnected";
  // share toggle (animated switch)
  RectF sw(380, 194, 40, 22);
  g_hots.push_back({RectF(372, 188, 170, 34), A_SHARE, -1, true});
  SolidBrush swb(g_share_knob > 0.5f ? rgba(170, 25, 25) : rgba(60, 54, 52));
  fill_round(g, sw, 11, swb);
  SolidBrush knob(rgba(240, 232, 226));
  g.FillEllipse(&knob, sw.X + 2 + 18 * g_share_knob, sw.Y + 2, 18.0f, 18.0f);
  text(g, L"Share my sounds", RectF(426, 188, 120, 34), 12, rgba(190, 178, 172));

  // main actions
  button(g, RectF(24, 236, 250, 46), idle ? L"HOST GAME" : L"HOSTING...", A_HOST, idle, true);
  button(g, RectF(286, 236, 250, 46), L"DISCONNECT", A_DISCONNECT, !idle, false);

  // server list
  text(g, L"SERVER LIST", RectF(24, 296, 200, 26), 13, rgba(225, 60, 55), FontStyleBold);
  bool busy = g_lobby && g_lobby->refreshing();
  button(g, RectF(W - 24 - 110.0f, 294, 110, 28), busy ? L"..." : L"REFRESH", A_REFRESH, !busy, false);
  RectF list(24, 328, (REAL)(W - 48), 220);
  SolidBrush lb2(rgba(0, 0, 0, 70));
  fill_round(g, list, 12, lb2);
  stroke_round(g, list, 12, rgba(255, 255, 255, 18), 1);
  g.SetClip(list);
  const float rowH = 52;
  if (g_servers.empty()) {
    std::wstring msg = busy ? L"Searching for games..." : L"No open games right now.\nHost one, or refresh in a moment.";
    if (g_lobby && !g_lobby->last_error().empty() && !busy) msg = to_wide(g_lobby->last_error());
    text(g, msg, list, 13.5f, rgba(150, 138, 132), FontStyleRegular, StringAlignmentCenter);
  }
  for (size_t i = 0; i < g_servers.size() && i < 64; ++i) {
    auto& e = g_servers[i];
    float slide = 1.0f - g_row_in[i];
    RectF row(list.X + 8 + 40 * slide, list.Y + 8 + i * rowH - g_scroll, list.Width - 16, rowH - 8);
    if (row.Y + row.Height < list.Y || row.Y > list.Y + list.Height) continue;
    int alpha = (int)(255 * g_row_in[i]);
    SolidBrush rb(rgba(255, 255, 255, (int)((14 + 22 * g_row_hover[i]) * g_row_in[i])));
    fill_round(g, row, 9, rb);
    if (g_row_hover[i] > 0.01f) stroke_round(g, row, 9, rgba(210, 40, 40, (int)(160 * g_row_hover[i])), 1.2f);
    // Versions with the same network protocol play together; others get an UPDATE button.
    bool compatible = e.proto ? e.proto == (int)kProtocolVersion : e.version == BTT_VERSION;
    bool same_ver = e.version == BTT_VERSION;
    text(g, to_wide(e.host_name), RectF(row.X + 14, row.Y + 3, row.Width - 170, 24), 15, rgba(240, 232, 226, alpha), FontStyleBold);
    std::wstring info = to_wide(short_map(e.map)) + L"  \x2022  " + std::to_wstring(e.players) + L"/" + std::to_wstring(e.max_players) +
                        L" players" + (same_ver ? L"" : L"  \x2022  v" + to_wide(e.version)) + (compatible ? L"" : L" \x2014 update needed");
    text(g, info, RectF(row.X + 14, row.Y + 23, row.Width - 170, 20), 12, compatible ? rgba(165, 152, 146, alpha) : rgba(230, 150, 70, alpha));
    button(g, RectF(row.X + row.Width - 104, row.Y + 7, 94, 30), compatible ? L"JOIN" : L"UPDATE",
           compatible ? A_JOIN_ROW : A_UPDATE_ROW, compatible ? idle : true, compatible, (int)i);
  }
  g.ResetClip();
  float maxScroll = std::max(0.0f, g_servers.size() * rowH + 8 - list.Height);
  g_scroll_target = std::clamp(g_scroll_target, 0.0f, maxScroll);

  // test helpers
  text(g, L"No one to play with? Try a simulated player:", RectF(24, 556, 400, 22), 12, rgba(150, 138, 132));
  button(g, RectF(24, 580, 160, 34), L"MIRROR ME", A_SIM_MIRROR, idle, false);
  button(g, RectF(194, 580, 160, 34), L"WALK AROUND", A_SIM_ORBIT, idle, false);
  if (dev_ui()) {
    button(g, RectF(364, 580, 84, 34), L"DEV HOST", A_DEV_HOST, idle, false);
    button(g, RectF(452, 580, 84, 34), L"DEV JOIN", A_DEV_JOIN, idle, false);
  }

  // update banner
  {
    std::lock_guard<std::mutex> lk(a.mu);
    if (a.update.available) {
      RectF ub(24, H - 22.0f - 0, (REAL)(W - 48), 18);
      g_hots.push_back({ub, A_UPDATE, -1, true});
      text(g, to_wide("Update available: v" + a.update.latest_version) + L"  \x2014  click to open the download page", ub, 12,
           rgba(245, 200, 90, 200 + (int)(55 * pulse)), FontStyleBold, StringAlignmentCenter);
    }
  }

  // toast
  if (now_sec() < g_toast_until) {
    float left = (float)(g_toast_until - now_sec());
    int al = (int)(255 * std::min(1.0f, left * 2));
    RectF tr(W / 2.0f - 200, 140, 400, 38);
    SolidBrush tb(rgba(140, 16, 16, (int)(al * 0.92)));
    fill_round(g, tr, 10, tb);
    text(g, g_toast, tr, 13.5f, rgba(255, 245, 240, al), FontStyleBold, StringAlignmentCenter);
  }

  // whole-window fade-in when shown
  if (g_open < 0.999f) {
    SolidBrush fade(rgba(9, 8, 8, (int)(255 * (1 - g_open))));
    g.FillRectangle(&fade, 0, 0, W, H);
  }

  Graphics screen(dc);
  screen.DrawImage(&bmp, 0, 0);
}

std::atomic<bool> g_quick_join{false};
double g_quick_join_since = 0;

// Server-list upkeep; runs even while the window is hidden (hosting from the watch menu).
void lobby_tick() {
  if (!g_lobby) return;
  App& a = app();
  EngineSnapshot s = a.engine->snapshot();
  bool open = s.role == "host" && s.state == "Waiting" && !s.join_code.empty() && s.transport.rfind("unity-relay", 0) == 0;
  g_lobby->set_listing(open, current_name(), short_map(s.local_map), s.join_code);
  if (g_quick_join && !g_lobby->refreshing() && g_lobby->last_refresh() > g_quick_join_since) {
    g_quick_join = false;
    for (auto& e : g_lobby->servers()) {
      bool compatible = e.proto ? e.proto == (int)kProtocolVersion : e.version == BTT_VERSION;
      if (compatible && e.players < e.max_players) {
        a.engine->join_relay(e.join_code);
        a.engine->notify_local("Joining " + e.host_name + "...");
        return;
      }
    }
    a.engine->notify_local(g_lobby->last_error().empty() ? "No open games found" : g_lobby->last_error());
  }
}

void tick() {
  App& a = app();
  EngineSnapshot s = a.engine->snapshot();
  double now = now_sec();
  if (g_lobby) {
    bool idle = s.state == "Idle" || s.state == "Disconnected";
    if (idle && a.window_visible && now - g_last_auto_refresh > 8.0) {
      g_last_auto_refresh = now;
      g_lobby->request_refresh();
    }
    auto list = g_lobby->servers();
    std::string sig;
    for (auto& e : list) sig += e.id + ",";
    if (sig != g_list_sig) {
      // keep the animation state of rows that stay; new rows slide in
      std::vector<float> in(64, 0.0f);
      for (size_t i = 0; i < list.size() && i < 64; ++i)
        for (size_t j = 0; j < g_servers.size() && j < 64; ++j)
          if (g_servers[j].id == list[i].id) in[i] = g_row_in[j];
      for (int i = 0; i < 64; ++i) { g_row_in[i] = in[i]; g_row_hover[i] = 0; }
      g_servers = list;
      g_list_sig = sig;
    }
  }
  // animations
  for (int i = 0; i < 16; ++i) {
    bool hot = false;
    for (auto& h : g_hots) if (h.act == i && h.row < 0 && h.enabled && inside(h.r, g_mouse_x, g_mouse_y)) hot = true;
    g_hover[i] = approach(g_hover[i], hot ? 1.0f : 0.0f, 0.18f);
  }
  for (int i = 0; i < 64; ++i) {
    bool hot = false;
    for (auto& h : g_hots) if (h.row == i && inside(h.r, g_mouse_x, g_mouse_y)) hot = true;
    g_row_hover[i] = approach(g_row_hover[i], hot ? 1.0f : 0.0f, 0.2f);
    if (i < (int)g_servers.size()) g_row_in[i] = approach(g_row_in[i], 1.0f, 0.12f - std::min(0.08f, i * 0.012f));
  }
  g_share_knob = approach(g_share_knob, g_share ? 1.0f : 0.0f, 0.25f);
  g_scroll = approach(g_scroll, g_scroll_target, 0.25f);
  g_open = approach(g_open, 1.0f, 0.12f);
  InvalidateRect(g_wnd, nullptr, FALSE);
}

void click(int x, int y) {
  NetEngine* e = app().engine.get();
  for (auto it = g_hots.rbegin(); it != g_hots.rend(); ++it) {
    if (!inside(it->r, x, y) || !it->enabled) continue;
    switch (it->act) {
      case A_HOST: apply_name(); e->host_relay(); toast(L"Creating your game..."); break;
      case A_DISCONNECT: e->disconnect(); toast(L"Left the game"); break;
      case A_REFRESH: if (g_lobby) g_lobby->request_refresh(); g_last_auto_refresh = now_sec(); break;
      case A_SHARE: g_share = !g_share; e->set_share_sounds(g_share); break;
      case A_SIM_MIRROR: apply_name(); e->start_simulated_peer(SimMode::Mirror); break;
      case A_SIM_ORBIT: apply_name(); e->start_simulated_peer(SimMode::Orbit); break;
      case A_DEV_HOST: apply_name(); e->host_dev_udp(27777); break;
      case A_DEV_JOIN: apply_name(); e->join_dev_udp("127.0.0.1", 27777); break;
      case A_UPDATE: {
        std::string url;
        {
          std::lock_guard<std::mutex> lk(app().mu);
          url = app().update.url;
        }
        if (url.rfind("https://github.com/", 0) == 0) ShellExecuteW(nullptr, L"open", to_wide(url).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        break;
      }
      case A_JOIN_ROW:
        if (it->row >= 0 && it->row < (int)g_servers.size()) {
          apply_name();
          e->join_relay(g_servers[it->row].join_code);
          toast(L"Joining " + to_wide(g_servers[it->row].host_name) + L"...");
        }
        break;
      case A_UPDATE_ROW:
        if (it->row >= 0 && it->row < (int)g_servers.size()) {
          ShellExecuteW(nullptr, L"open", L"https://github.com/doktordestrukt/BloodTrailTogether/releases/latest", nullptr, nullptr, SW_SHOWNORMAL);
          toast(to_wide(g_servers[it->row].host_name) + L" runs v" + to_wide(g_servers[it->row].version) + L" \x2014 update so you match");
        }
        break;
      default: break;
    }
    return;
  }
}

LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM w, LPARAM l) {
  switch (m) {
    case WM_PAINT: {
      PAINTSTRUCT ps;
      HDC dc = BeginPaint(h, &ps);
      draw(dc);
      EndPaint(h, &ps);
      return 0;
    }
    case WM_ERASEBKGND: return 1;
    case WM_CTLCOLOREDIT:
      SetTextColor((HDC)w, RGB(240, 232, 226));
      SetBkColor((HDC)w, RGB(20, 14, 14));
      return (LRESULT)g_edit_bg;
    case WM_TIMER:
      lobby_tick();
      if (app().window_visible) tick();
      return 0;
    case WM_APP + 2:  // watch menu: host
      apply_name();
      app().engine->host_relay();
      app().engine->notify_local("Hosting - your game is in the server list");
      return 0;
    case WM_APP + 3:  // watch menu: quick join
      if (!g_lobby) { app().engine->notify_local("Server list unavailable (no Unity project id)"); return 0; }
      apply_name();
      g_quick_join_since = now_sec();
      g_quick_join = true;
      g_lobby->request_refresh();
      app().engine->notify_local("Looking for an open game...");
      return 0;
    case WM_MOUSEMOVE: {
      g_mouse_x = GET_X_LPARAM(l);
      g_mouse_y = GET_Y_LPARAM(l);
      TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, h, 0};
      TrackMouseEvent(&tme);
      bool hand = false;
      for (auto& hot : g_hots) if (hot.enabled && inside(hot.r, g_mouse_x, g_mouse_y)) hand = true;
      SetCursor(LoadCursorW(nullptr, hand ? IDC_HAND : IDC_ARROW));
      return 0;
    }
    case WM_SETCURSOR:
      if (LOWORD(l) == HTCLIENT) return TRUE;  // set in WM_MOUSEMOVE
      break;
    case WM_MOUSELEAVE: g_mouse_x = g_mouse_y = -1; g_mouse_down = false; return 0;
    case WM_LBUTTONDOWN: g_mouse_down = true; SetCapture(h); SetFocus(h); return 0;
    case WM_LBUTTONUP:
      if (g_mouse_down) click(GET_X_LPARAM(l), GET_Y_LPARAM(l));
      g_mouse_down = false;
      ReleaseCapture();
      return 0;
    case WM_MOUSEWHEEL: g_scroll_target -= GET_WHEEL_DELTA_WPARAM(w) / 120.0f * 52.0f; return 0;
    case WM_COMMAND:
      if (LOWORD(w) == ID_NAME && HIWORD(w) == EN_KILLFOCUS) apply_name();
      return 0;
    case WM_CLOSE:  // closing just hides; the game keeps the mod running
      ShowWindow(h, SW_HIDE);
      app().window_visible = false;
      return 0;
    case WM_APP + 1:
      app().window_visible = !app().window_visible;
      if (app().window_visible) { g_open = 0; g_last_auto_refresh = 0; }
      ShowWindow(h, app().window_visible ? SW_SHOWNOACTIVATE : SW_HIDE);
      return 0;
  }
  return DefWindowProcW(h, m, w, l);
}

DWORD WINAPI ui_thread(LPVOID) {
  GdiplusStartupInput gsi;
  GdiplusStartup(&g_gdip, &gsi, nullptr);
  g_t0 = now_sec();
  {
    Settings st = app().engine->settings();
    if (!st.unity_project_id.empty()) g_lobby = std::make_unique<LobbyClient>(st.unity_project_id, st.unity_environment);
  }
  g_edit_bg = CreateSolidBrush(RGB(20, 14, 14));
  g_name_font = CreateFontW(-16, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");

  WNDCLASSW wc{};
  wc.lpfnWndProc = wndproc;
  wc.hInstance = GetModuleHandleW(nullptr);
  wc.lpszClassName = L"BloodTrailTogetherPanel2";
  wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  RegisterClassW(&wc);

  RECT r{0, 0, W, H};
  DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN;
  AdjustWindowRect(&r, style, FALSE);
  g_wnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, wc.lpszClassName, L"BloodTrail Together", style, 40, 40,
                          r.right - r.left, r.bottom - r.top, nullptr, nullptr, wc.hInstance, nullptr);
  g_name = CreateWindowExW(0, L"EDIT", to_wide(app().settings.player_name).c_str(), WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                           142, 195, 200, 22, g_wnd, (HMENU)(INT_PTR)ID_NAME, nullptr, nullptr);
  SendMessageW(g_name, WM_SETFONT, (WPARAM)g_name_font, TRUE);
  SendMessageW(g_name, EM_SETLIMITTEXT, 24, 0);

  ShowWindow(g_wnd, SW_SHOWNOACTIVATE);
  SetTimer(g_wnd, TIMER_ANIM, 16, nullptr);
  MSG msg;
  while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
    if (msg.message == WM_KEYDOWN && msg.wParam == VK_RETURN && msg.hwnd == g_name) { apply_name(); SetFocus(g_wnd); continue; }
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }
  g_lobby.reset();
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

void control_window_host() {
  if (g_wnd) PostMessageW(g_wnd, WM_APP + 2, 0, 0);
}

void control_window_quick_join() {
  if (g_wnd) PostMessageW(g_wnd, WM_APP + 3, 0, 0);
}

}  // namespace btt
