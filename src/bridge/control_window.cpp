// "BloodTrail Together" multiplayer panel (F9 in game). Fully custom-drawn with GDI+ and animated
// at ~60 fps while visible: hover glows, pressed states, pulsing connection light, sliding server
// rows, fading toasts. Hosting puts your game in a public SERVER LIST (Unity Lobby); joining is a
// click on a row - no codes to type.
#include <windows.h>
#include <windowsx.h>
#include <objidl.h>
#include <gdiplus.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <commctrl.h>

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

enum Action { A_NONE, A_HOST, A_REFRESH, A_DISCONNECT, A_SHARE, A_SIM_MIRROR, A_SIM_ORBIT, A_UPDATE, A_DEV_HOST, A_DEV_JOIN, A_JOIN_ROW, A_UPDATE_ROW,
              A_CHAR_OPEN, A_CHAR_DONE, A_CPREV, A_CNEXT, A_OUTFIT_SAVE, A_OUTFIT_LOAD, A_OUTFIT_DEL, A_THIRD };

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
float g_hover[32] = {};       // per-action hover amount 0..1
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

// ---------------------------------------------------------------------------------------------
// CHARACTER panel: live 3D preview (rendered by the game into preview.png), slot/colour pickers,
// animated name effects. Slides in over the main view.
struct WItem {
  std::string name;
  std::vector<std::string> colours;
};
std::vector<WItem> g_wd[5];               // top, bottom, shoes, hat, hair
std::vector<std::string> g_fx_names;
size_t g_wd_lines = 0;
std::vector<int> g_look;                  // 10 values, 1-based
float g_char_t = 0;                       // panel slide 0..1
bool g_char_open = false;
bool g_dragging = false;
int g_drag_x = 0;
double g_last_drag = -100;
std::unique_ptr<Gdiplus::Bitmap> g_preview;
FILETIME g_preview_time{};
float g_row_flash[9] = {};

const char* kSlotNames[5] = {"top", "bottom", "shoes", "hat", "hair"};

void wardrobe_sync() {
  App& a = app();
  std::lock_guard<std::mutex> lk(a.mu);
  if (a.wardrobe.size() != g_wd_lines) {
    g_wd_lines = a.wardrobe.size();
    for (auto& v : g_wd) v.clear();
    g_fx_names.clear();
    for (auto& l : a.wardrobe) {
      // "<slot> <idx> <name>|<c1>;<c2>"   or   "fx 0 <n1>;<n2>"
      size_t s1 = l.find(' '), s2 = l.find(' ', s1 + 1);
      if (s1 == std::string::npos || s2 == std::string::npos) continue;
      std::string slot = l.substr(0, s1), rest = l.substr(s2 + 1);
      auto split = [](const std::string& s, char sep) {
        std::vector<std::string> out;
        size_t p = 0;
        while (p <= s.size()) {
          size_t q = s.find(sep, p);
          if (q == std::string::npos) q = s.size();
          if (q > p) out.push_back(s.substr(p, q - p));
          p = q + 1;
        }
        return out;
      };
      if (slot == "fx") { g_fx_names = split(rest, ';'); continue; }
      for (int i = 0; i < 5; ++i)
        if (slot == kSlotNames[i]) {
          size_t bar = rest.find('|');
          WItem it;
          it.name = rest.substr(0, bar);
          if (bar != std::string::npos) it.colours = split(rest.substr(bar + 1), ';');
          g_wd[i].push_back(it);
        }
    }
  }
  if (g_look.size() != 10) {
    g_look.clear();
    for (size_t p = 0; p < a.look.size();) {
      while (p < a.look.size() && a.look[p] == ' ') ++p;
      if (p >= a.look.size()) break;
      g_look.push_back(atoi(a.look.c_str() + p));
      while (p < a.look.size() && a.look[p] != ' ') ++p;
    }
    if (g_look.size() != 10) g_look.clear();
  }
}

void look_commit() {
  std::string s;
  for (size_t i = 0; i < g_look.size(); ++i) s += (i ? " " : "") + std::to_string(g_look[i]);
  std::lock_guard<std::mutex> lk(app().mu);
  app().look = s;
}

// row r -> (look index, list size); row 4 = shoes colour
int row_index(int r) { static const int idx[9] = {0, 1, 2, 3, 5, 6, 7, 8, 9}; return idx[r]; }
int row_count(int r) {
  if (g_look.size() != 10) return 0;
  auto item = [&](int slot, int li) -> const WItem* {
    int i = g_look[li] - 1;
    return (i >= 0 && i < (int)g_wd[slot].size()) ? &g_wd[slot][i] : nullptr;
  };
  switch (r) {
    case 0: return (int)g_wd[0].size();
    case 1: { auto it = item(0, 0); return it ? (int)it->colours.size() : 0; }
    case 2: return (int)g_wd[1].size();
    case 3: { auto it = item(1, 2); return it ? (int)it->colours.size() : 0; }
    case 4: return g_wd[2].size() > 1 ? 1 + (int)g_wd[2][1].colours.size() : 1;
    case 5: return (int)g_wd[3].size();
    case 6: { auto it = item(3, 6); return it ? (int)it->colours.size() : 0; }
    case 7: return (int)g_wd[4].size();
    case 8: return (int)g_fx_names.size();
  }
  return 0;
}
std::string row_value(int r) {
  if (g_look.size() != 10) return "-";
  int v = g_look[row_index(r)] - 1;
  auto name_of = [&](int slot) { return (v >= 0 && v < (int)g_wd[slot].size()) ? g_wd[slot][v].name : std::string("-"); };
  auto colour_of = [&](int slot, int li) {
    int i = g_look[li] - 1;
    if (i < 0 || i >= (int)g_wd[slot].size() || g_wd[slot][i].colours.empty()) return std::string("\xE2\x80\x94");
    const auto& cs = g_wd[slot][i].colours;
    return (v >= 0 && v < (int)cs.size()) ? cs[v] : cs[0];
  };
  switch (r) {
    case 0: return name_of(0);
    case 1: return colour_of(0, 0);
    case 2: return name_of(1);
    case 3: return colour_of(1, 2);
    case 4: {
      if (g_look[4] <= 1 || g_wd[2].size() < 2) return "None";
      const auto& cs = g_wd[2][1].colours;
      int c = g_look[5] - 1;
      return "Sneakers \xC2\xB7 " + ((c >= 0 && c < (int)cs.size()) ? cs[c] : std::string("-"));
    }
    case 5: return name_of(3);
    case 6: return colour_of(3, 6);
    case 7: return name_of(4);
    case 8: return (v >= 0 && v < (int)g_fx_names.size()) ? g_fx_names[v] : "Classic";
  }
  return "-";
}
void row_step(int r, int d) {
  int n = row_count(r);
  if (n <= 0 || g_look.size() != 10) return;
  if (r == 4) {  // shoes: 0 = none, 1..n = sneakers in colour n
    int c = g_look[4] <= 1 ? 0 : g_look[5];
    c = ((c + d) % n + n) % n;
    g_look[4] = c == 0 ? 1 : 2;
    g_look[5] = c == 0 ? 1 : c;
    g_row_flash[r] = 1.0f;
    look_commit();
    return;
  }
  int li = row_index(r);
  int v = g_look[li] - 1;
  v = ((v + d) % n + n) % n;
  g_look[li] = v + 1;
  if (r == 0 || r == 2 || r == 5) g_look[li + 1] = 1;  // new item: first colour
  g_row_flash[r] = 1.0f;
  look_commit();
}

// same colours as the game script's name effects (EX.fxColour)
Color fx_colour(int fx, int i, int n, double t, float& dy) {
  dy = 0;
  float u = n > 1 ? (float)(i) / (n - 1) : 0;
  auto hsv = [](double h, double s, double v) {
    h = fmod(fmod(h, 1.0) + 1.0, 1.0) * 6;
    int k = (int)h;
    double f = h - k, p = v * (1 - s), q = v * (1 - s * f), tt = v * (1 - s * (1 - f));
    double r = v, g = tt, b = p;
    switch (k) {
      case 1: r = q; g = v; b = p; break;
      case 2: r = p; g = v; b = tt; break;
      case 3: r = p; g = q; b = v; break;
      case 4: r = tt; g = p; b = v; break;
      case 5: r = v; g = p; b = q; break;
    }
    return Color(255, (BYTE)(r * 255), (BYTE)(g * 255), (BYTE)(b * 255));
  };
  switch (fx) {
    case 2: return hsv(t * 0.25 + u * 0.6, 0.85, 1);
    case 3: {
      static const int a[4][3] = {{255, 140, 40}, {255, 60, 120}, {150, 60, 255}, {255, 140, 40}};
      double k = fmod(u + t * 0.15, 1.0) * 3;
      int j = (int)k;
      double f = k - j;
      return Color(255, (BYTE)(a[j][0] + (a[j + 1][0] - a[j][0]) * f), (BYTE)(a[j][1] + (a[j + 1][1] - a[j][1]) * f),
                   (BYTE)(a[j][2] + (a[j + 1][2] - a[j][2]) * f));
    }
    case 4: { double f = (rand() % 1000) / 1000.0; return Color(255, 255, (BYTE)(60 + 170 * f * (0.6 + 0.4 * sin(t * 9 + i))), (BYTE)(20 * f)); }
    case 5: { double s = 0.5 + 0.5 * sin(t * 3 + i * 0.9); return Color(255, (BYTE)(120 + 135 * s), (BYTE)(210 + 45 * s), 255); }
    case 6: {
      double sweep = std::max(0.0, 1 - fabs(fmod(t * 0.6, 1.6) - 0.3 - u) * 5);
      return Color(255, 255, (BYTE)(190 + 65 * sweep), (BYTE)(40 + 215 * sweep));
    }
    case 7: { double s = 0.5 + 0.5 * sin(t * 4 - u * 4); return Color(255, (BYTE)(80 + 100 * s), 255, (BYTE)(40 + 60 * s)); }
    case 8: { double s = 0.5 + 0.5 * sin(t * 5); return Color(255, (BYTE)(150 + 105 * s), (BYTE)(20 * s), (BYTE)(20 * s)); }
    case 9:
      if (rand() % 100 < 8) { dy = (float)((rand() % 100) / 100.0 - 0.5) * 6; return hsv((rand() % 100) / 100.0, 1, 1); }
      return Color(255, 235, 235, 235);
    case 10: dy = (float)(sin(t * 5 - i * 0.7) * 4); return hsv(t * 0.2 + u * 0.5, 0.75, 1);
  }
  return Color(255, 235, 225, 215);
}

void preview_reload() {
  std::wstring path = app().mod_dir + L"\\preview.png";
  WIN32_FILE_ATTRIBUTE_DATA fa;
  if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fa)) return;
  if (CompareFileTime(&fa.ftLastWriteTime, &g_preview_time) == 0) return;
  HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
  if (f == INVALID_HANDLE_VALUE) return;
  DWORD size = GetFileSize(f, nullptr), got = 0;
  std::vector<BYTE> buf(size);
  BOOL ok = size > 0 && ReadFile(f, buf.data(), size, &got, nullptr) && got == size;
  CloseHandle(f);
  if (!ok) return;
  IStream* st = SHCreateMemStream(buf.data(), size);
  if (!st) return;
  std::unique_ptr<Bitmap> src(Bitmap::FromStream(st));
  st->Release();
  if (!src || src->GetLastStatus() != Ok || src->GetWidth() == 0) return;  // half-written file: try again next tick
  g_preview_time = fa.ftLastWriteTime;
  // copy out with alpha forced opaque (scene captures store inverted / empty alpha)
  auto out = std::make_unique<Bitmap>(src->GetWidth(), src->GetHeight(), PixelFormat32bppARGB);
  Rect r(0, 0, src->GetWidth(), src->GetHeight());
  BitmapData sd, dd;
  if (src->LockBits(&r, ImageLockModeRead, PixelFormat32bppARGB, &sd) == Ok) {
    if (out->LockBits(&r, ImageLockModeWrite, PixelFormat32bppARGB, &dd) == Ok) {
      for (UINT y = 0; y < (UINT)r.Height; ++y) {
        auto* s = (uint32_t*)((BYTE*)sd.Scan0 + y * sd.Stride);
        auto* d = (uint32_t*)((BYTE*)dd.Scan0 + y * dd.Stride);
        for (UINT x = 0; x < (UINT)r.Width; ++x) d[x] = s[x] | 0xFF000000u;
      }
      out->UnlockBits(&dd);
    }
    src->UnlockBits(&sd);
  }
  g_preview = std::move(out);
}

const RectF kPreview(24, 92, 236, 360);

// ---- saved outfits: "name|look" lines in outfits.txt next to the mod
struct Outfit { std::string name, look; };
std::vector<Outfit> g_outfits;
HWND g_outfit_edit = nullptr;
float g_outfit_scroll = 0;

std::wstring outfits_path() { return app().mod_dir + L"\\outfits.txt"; }
void outfits_load() {
  g_outfits.clear();
  FILE* f = nullptr;
  if (_wfopen_s(&f, outfits_path().c_str(), L"r") != 0 || !f) return;
  char line[512];
  while (fgets(line, sizeof(line), f)) {
    std::string l = line;
    while (!l.empty() && (l.back() == '\n' || l.back() == '\r')) l.pop_back();
    size_t bar = l.find('|');
    if (bar != std::string::npos && bar > 0) g_outfits.push_back({l.substr(0, bar), l.substr(bar + 1)});
  }
  fclose(f);
}
void outfits_save() {
  FILE* f = nullptr;
  if (_wfopen_s(&f, outfits_path().c_str(), L"w") != 0 || !f) return;
  for (auto& o : g_outfits) fprintf(f, "%s|%s\n", o.name.c_str(), o.look.c_str());
  fclose(f);
}
std::string look_string() {
  std::string s;
  for (size_t i = 0; i < g_look.size(); ++i) s += (i ? " " : "") + std::to_string(g_look[i]);
  return s;
}
void outfit_save_current() {
  wchar_t buf[64] = {};
  GetWindowTextW(g_outfit_edit, buf, 64);
  std::string name = to_utf8(buf);
  for (auto& c : name) if (c == '|') c = '/';
  if (name.empty()) name = "Outfit " + std::to_string(g_outfits.size() + 1);
  for (auto& o : g_outfits)
    if (o.name == name) { o.look = look_string(); outfits_save(); toast(L"Outfit updated: " + to_wide(name)); return; }
  g_outfits.push_back({name, look_string()});
  outfits_save();
  SetWindowTextW(g_outfit_edit, L"");
  toast(L"Outfit saved: " + to_wide(name));
}
void outfit_load(int i) {
  if (i < 0 || i >= (int)g_outfits.size()) return;
  std::vector<int> v;
  const std::string& s = g_outfits[i].look;
  for (size_t p = 0; p < s.size();) {
    while (p < s.size() && s[p] == ' ') ++p;
    if (p >= s.size()) break;
    v.push_back(atoi(s.c_str() + p));
    while (p < s.size() && s[p] != ' ') ++p;
  }
  if (v.size() != 10) return;
  g_look = v;
  for (auto& f : g_row_flash) f = 1.0f;
  look_commit();
  toast(L"Wearing: " + to_wide(g_outfits[i].name));
}


void draw_char_panel(Graphics& g, double t) {
  g_hots.clear();
  float e = 1 - powf(1 - g_char_t, 3);
  float ox = (1 - e) * W;
  g.TranslateTransform(ox, 0);
  LinearGradientBrush bg(PointF(0, 0), PointF(0, (REAL)H), rgba(24, 10, 10), rgba(8, 7, 7));
  g.FillRectangle(&bg, 0, 0, W, H);
  text(g, L"CHARACTER", RectF(24, 18, 300, 34), 26, rgba(235, 228, 220), FontStyleBold, StringAlignmentNear, StringAlignmentCenter, L"Segoe UI Black");
  text(g, L"How the other player sees you. Saved automatically.", RectF(24, 50, 480, 20), 12, rgba(150, 135, 130));
  {
    LinearGradientBrush lb(PointF(0, 0), PointF((REAL)W, 0), rgba(120, 10, 10, 0), rgba(120, 10, 10, 0));
    Color cols[] = {rgba(120, 10, 10, 0), rgba(220, 30, 30, 230), rgba(120, 10, 10, 0)};
    REAL pos[] = {0.0f, std::clamp((float)fmod(t * 0.35, 1.0), 0.01f, 0.99f), 1.0f};
    lb.SetInterpolationColors(cols, pos, 3);
    g.FillRectangle(&lb, 24.0f, 74.0f, (REAL)(W - 48), 2.0f);
  }
  // preview stage
  RectF pr = kPreview;
  LinearGradientBrush stage(PointF(pr.X, pr.Y), PointF(pr.X, pr.Y + pr.Height), rgba(40, 14, 14), rgba(6, 5, 5));
  fill_round(g, pr, 14, stage);
  if (g_preview) {
    GraphicsPath clip;
    round_rect(clip, pr, 14);
    g.SetClip(&clip);
    float iw = (float)g_preview->GetWidth(), ih = (float)g_preview->GetHeight();
    float sc = std::max(pr.Width / iw, pr.Height / ih);
    g.DrawImage(g_preview.get(), RectF(pr.X + (pr.Width - iw * sc) / 2, pr.Y + (pr.Height - ih * sc) / 2, iw * sc, ih * sc));
    g.ResetClip();
  } else {
    text(g, L"Loading preview...\n(start the game first)", pr, 12.5f, rgba(150, 138, 132), FontStyleRegular, StringAlignmentCenter);
  }
  float pulse = (float)(0.5 + 0.5 * sin(t * 2.2));
  stroke_round(g, pr, 14, rgba(210, 35, 35, 60 + (int)(90 * pulse)), 1.6f);
  text(g, L"\x27F2  drag to rotate", RectF(pr.X, pr.Y + pr.Height + 4, pr.Width, 18), 11, rgba(140, 128, 122), FontStyleRegular, StringAlignmentCenter);

  // pickers
  static const wchar_t* labels[9] = {L"TOP", L"COLOUR", L"BOTTOM", L"COLOUR", L"SHOES", L"HAT", L"COLOUR", L"HAIR", L"NAME EFFECT"};
  for (int r = 0; r < 9; ++r) {
    float y = 92 + r * 40.0f;
    RectF row(278, y, 258, 34);
    g_row_flash[r] = std::max(0.0f, g_row_flash[r] - 0.04f);
    SolidBrush rb(rgba(255, 255, 255, 10 + (int)(40 * g_row_flash[r])));
    fill_round(g, row, 8, rb);
    text(g, labels[r], RectF(row.X + 10, y, 90, 34), 10.5f, rgba(225, 60, 55), FontStyleBold);
    int n = row_count(r);
    text(g, to_wide(row_value(r)), RectF(row.X + 92, y, 120, 34), 13, n > 0 ? rgba(240, 232, 226) : rgba(110, 100, 98), FontStyleBold, StringAlignmentCenter);
    // arrows (hover glow handled by button())
    RectF lb(row.X + 206, y + 4, 24, 26), rb2(row.X + 232, y + 4, 24, 26);
    g.TranslateTransform(-ox, 0);  // hit boxes in window coordinates
    lb.X += ox; rb2.X += ox;
    button(g, lb, L"\x2039", A_CPREV, n > 1, false, 40 + r);
    button(g, rb2, L"\x203A", A_CNEXT, n > 1, false, 40 + r);
    g.TranslateTransform(ox, 0);
  }
  // name with its effect
  RectF nr(278, 460, 258, 48);
  SolidBrush nb(rgba(0, 0, 0, 90));
  fill_round(g, nr, 10, nb);
  int fx = g_look.size() == 10 ? g_look[9] : 1;
  std::wstring name = to_wide(current_name());
  if (name.size() > 16) name.resize(16);
  FontFamily ff(L"Segoe UI Black");
  Font f(&ff, 20, FontStyleBold, UnitPixel);
  float cw = 15.0f, total = cw * name.size();
  for (size_t i = 0; i < name.size(); ++i) {
    float dy;
    Color c = fx_colour(fx, (int)i, (int)name.size(), t, dy);
    SolidBrush b(c);
    wchar_t ch[2] = {name[i], 0};
    StringFormat sf;
    sf.SetAlignment(StringAlignmentCenter);
    sf.SetLineAlignment(StringAlignmentCenter);
    g.DrawString(ch, 1, &f, RectF(nr.X + (nr.Width - total) / 2 + i * cw, nr.Y + dy, cw, nr.Height), &sf, &b);
  }
  // outfit name + save
  RectF ef(278, 520, 170, 32);
  SolidBrush eb(rgba(0, 0, 0, 90));
  fill_round(g, ef, 8, eb);
  stroke_round(g, ef, 8, rgba(200, 40, 40, GetFocus() == g_outfit_edit ? 200 : 70), 1.2f);
  g.TranslateTransform(-ox, 0);
  button(g, RectF(454 + ox, 520, 82, 32), L"SAVE", A_OUTFIT_SAVE, true, false);
  button(g, RectF(W - 24 - 258.0f + ox, 566, 258, 46), L"DONE", A_CHAR_DONE, true, true);
  g.TranslateTransform(ox, 0);
  // saved outfits (left, under the preview)
  text(g, L"OUTFITS", RectF(24, 474, 120, 20), 11, rgba(225, 60, 55), FontStyleBold);
  RectF ol(24, 496, 236, 116);
  SolidBrush olb(rgba(0, 0, 0, 70));
  fill_round(g, ol, 10, olb);
  if (g_outfits.empty())
    text(g, L"Name an outfit and press SAVE", ol, 11.5f, rgba(140, 128, 122), FontStyleRegular, StringAlignmentCenter);
  float maxs = std::max(0.0f, g_outfits.size() * 28.0f + 6 - ol.Height);
  g_outfit_scroll = std::clamp(g_outfit_scroll, 0.0f, maxs);
  g.SetClip(ol);
  for (size_t i = 0; i < g_outfits.size() && i < 14; ++i) {
    RectF r(ol.X + 6, ol.Y + 6 + i * 28.0f - g_outfit_scroll, ol.Width - 12, 24);
    if (r.Y + r.Height < ol.Y || r.Y > ol.Y + ol.Height) continue;
    g.TranslateTransform(-ox, 0);
    button(g, RectF(r.X + ox, r.Y, r.Width - 30, r.Height), to_wide(g_outfits[i].name), A_OUTFIT_LOAD, true, false, 50 + (int)i);
    button(g, RectF(r.X + r.Width - 26 + ox, r.Y, 26, r.Height), L"\x2715", A_OUTFIT_DEL, true, false, 50 + (int)i);
    g.TranslateTransform(ox, 0);
  }
  g.ResetClip();
  g.ResetTransform();
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
  button(g, RectF(24, 236, 124, 46), idle ? L"HOST GAME" : s.role == "host" ? L"HOSTING" : L"IN GAME", A_HOST, idle, true);
  button(g, RectF(154, 236, 124, 46), L"CHARACTER", A_CHAR_OPEN, true, false);
  button(g, RectF(284, 236, 124, 46), a.third_person ? L"3RD PERSON '13" : L"3RD PERSON", A_THIRD, true, a.third_person.load());
  button(g, RectF(414, 236, 122, 46), L"DISCONNECT", A_DISCONNECT, !idle, false);

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

  if (g_char_t > 0.001f) draw_char_panel(g, t);

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
  for (int i = 0; i < 32; ++i) {
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
  wardrobe_sync();
  if (dev_ui() && !g_char_open && now - g_t0 > 12 && now - g_t0 < 12.1 &&
      a.config.get("Dev.SelfTest").find("charpanel") != std::string::npos) {  // developer: open the panel for screenshots
    g_char_open = true;
    a.char_open = true;
    outfits_load();
    ShowWindow(g_name, SW_HIDE);
    ShowWindow(g_outfit_edit, SW_SHOW);
  }
  g_char_t = approach(g_char_t, g_char_open ? 1.0f : 0.0f, 0.14f);
  if (g_char_open) {
    preview_reload();
    if (!g_dragging && now - g_last_drag > 3.0) a.char_yaw = fmodf(a.char_yaw + 0.35f, 360.0f);  // turntable
  }
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
      case A_CHAR_OPEN:
        g_char_open = true;
        app().char_open = true;
        outfits_load();
        ShowWindow(g_name, SW_HIDE);
        ShowWindow(g_outfit_edit, SW_SHOW);
        break;
      case A_CHAR_DONE:
        g_char_open = false;
        app().char_open = false;
        ShowWindow(g_outfit_edit, SW_HIDE);
        ShowWindow(g_name, SW_SHOW);
        toast(L"Look saved");
        break;
      case A_CPREV: row_step(it->row - 40, -1); break;
      case A_CNEXT: row_step(it->row - 40, 1); break;
      case A_OUTFIT_SAVE: outfit_save_current(); break;
      case A_OUTFIT_LOAD: outfit_load(it->row - 50); break;
      case A_OUTFIT_DEL:
        if (it->row - 50 >= 0 && it->row - 50 < (int)g_outfits.size()) {
          toast(L"Deleted: " + to_wide(g_outfits[it->row - 50].name));
          g_outfits.erase(g_outfits.begin() + (it->row - 50));
          outfits_save();
        }
        break;
      case A_THIRD:
        app().third_person = !app().third_person;
        toast(app().third_person ? L"Third person: over the right shoulder" : L"First person");
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
      if (g_dragging) {
        app().char_yaw = fmodf(app().char_yaw + (g_mouse_x - g_drag_x) * 0.6f + 360.0f, 360.0f);
        g_drag_x = g_mouse_x;
        g_last_drag = now_sec();
      }
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
    case WM_LBUTTONDOWN:
      g_mouse_down = true;
      SetCapture(h);
      SetFocus(h);
      if (g_char_open && inside(kPreview, GET_X_LPARAM(l), GET_Y_LPARAM(l))) {
        g_dragging = true;
        g_drag_x = GET_X_LPARAM(l);
        g_last_drag = now_sec();
      }
      return 0;
    case WM_LBUTTONUP:
      if (g_mouse_down && !g_dragging) click(GET_X_LPARAM(l), GET_Y_LPARAM(l));
      g_mouse_down = false;
      g_dragging = false;
      ReleaseCapture();
      return 0;
    case WM_MOUSEWHEEL:
      if (g_char_open) g_outfit_scroll -= GET_WHEEL_DELTA_WPARAM(w) / 120.0f * 28.0f;
      else g_scroll_target -= GET_WHEEL_DELTA_WPARAM(w) / 120.0f * 52.0f;
      return 0;
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
  g_outfit_edit = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | ES_AUTOHSCROLL, 288, 525, 152, 22, g_wnd, (HMENU)(INT_PTR)(ID_NAME + 1),
                                  nullptr, nullptr);
  SendMessageW(g_outfit_edit, WM_SETFONT, (WPARAM)g_name_font, TRUE);
  SendMessageW(g_outfit_edit, EM_SETLIMITTEXT, 30, 0);
  SendMessageW(g_outfit_edit, EM_SETCUEBANNER, TRUE, (LPARAM)L"Outfit name");

  ShowWindow(g_wnd, SW_SHOWNOACTIVATE);
  SetTimer(g_wnd, TIMER_ANIM, 16, nullptr);
  MSG msg;
  while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
    if (msg.message == WM_KEYDOWN && msg.wParam == VK_RETURN && msg.hwnd == g_name) { apply_name(); SetFocus(g_wnd); continue; }
    if (msg.message == WM_KEYDOWN && msg.wParam == VK_RETURN && msg.hwnd == g_outfit_edit) { outfit_save_current(); SetFocus(g_wnd); continue; }
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
