#include "install_core.h"

#include <windows.h>
#include <shlobj.h>
#include <tlhelp32.h>

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>

namespace fs = std::filesystem;

namespace bttinst {

std::string narrow(const std::wstring& w) {
  if (w.empty()) return {};
  int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
  std::string s(n, '\0');
  WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
  return s;
}

std::wstring widen(const std::string& s) {
  if (s.empty()) return {};
  int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
  std::wstring w(n, L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
  return w;
}

// ------------------------------------------------------------------ payload
bool Payload::has_ue4ss() const {
  for (auto& f : files)
    if (f.component == 2) return true;
  return false;
}

std::string Payload::version() const {
  auto it = meta.find("version");
  return it == meta.end() ? "?" : it->second;
}

bool parse_payload(const unsigned char* d, size_t n, Payload& out, std::string& err) {
  size_t p = 0;
  auto need = [&](size_t k) { return p + k <= n; };
  auto u32 = [&]() { uint32_t v; memcpy(&v, d + p, 4); p += 4; return v; };
  auto u16 = [&]() { uint16_t v; memcpy(&v, d + p, 2); p += 2; return v; };
  if (!need(12) || memcmp(d, "BTTPACK1", 8) != 0) { err = "installer payload is missing or damaged"; return false; }
  p = 8;
  uint32_t count = u32();
  out = Payload{};
  for (uint32_t i = 0; i < count; ++i) {
    if (!need(3)) { err = "payload truncated"; return false; }
    int comp = d[p++];
    uint16_t plen = u16();
    if (!need(plen + 4)) { err = "payload truncated"; return false; }
    std::string path((const char*)d + p, plen);
    p += plen;
    uint32_t size = u32();
    if (!need(size)) { err = "payload truncated"; return false; }
    if (comp == 0) {
      std::istringstream in(std::string((const char*)d + p, size));
      std::string line;
      while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        auto eq = line.find('=');
        if (eq != std::string::npos) out.meta[line.substr(0, eq)] = line.substr(eq + 1);
      }
    } else {
      PayloadFile f;
      f.component = comp;
      std::replace(path.begin(), path.end(), '/', '\\');
      f.rel_path = widen(path);
      f.data.assign(d + p, d + p + size);
      out.files.push_back(std::move(f));
    }
    p += size;
  }
  return true;
}

bool load_payload_resource(Payload& out, std::string& err) {
  HRSRC r = FindResourceW(nullptr, MAKEINTRESOURCEW(101), RT_RCDATA);
  if (!r) { err = "this installer contains no payload (developer build?)"; return false; }
  HGLOBAL g = LoadResource(nullptr, r);
  auto* d = (const unsigned char*)LockResource(g);
  return parse_payload(d, SizeofResource(nullptr, r), out, err);
}

bool load_payload_file(const std::wstring& path, Payload& out, std::string& err) {
  std::ifstream f(path, std::ios::binary);
  if (!f) { err = "cannot open payload file"; return false; }
  std::vector<unsigned char> buf((std::istreambuf_iterator<char>(f)), {});
  return parse_payload(buf.data(), buf.size(), out, err);
}

// ------------------------------------------------------------------ Steam detection
static std::wstring reg_str(HKEY root, const wchar_t* key, const wchar_t* val) {
  wchar_t buf[1024];
  DWORD sz = sizeof(buf);
  if (RegGetValueW(root, key, val, RRF_RT_REG_SZ, nullptr, buf, &sz) == ERROR_SUCCESS) return buf;
  return {};
}

static std::string read_text(const fs::path& p) {
  std::ifstream f(p, std::ios::binary);
  if (!f) return {};
  std::stringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

// Extracts all values of "key" "value" pairs with the given key from a Valve KeyValues text.
static std::vector<std::string> vdf_values(const std::string& text, const std::string& key) {
  std::vector<std::string> out;
  std::string needle = "\"" + key + "\"";
  size_t pos = 0;
  while ((pos = text.find(needle, pos)) != std::string::npos) {
    pos += needle.size();
    size_t a = text.find('"', pos);
    size_t nl = text.find('\n', pos);
    if (a == std::string::npos || (nl != std::string::npos && a > nl)) continue;
    std::string v;
    for (size_t i = a + 1; i < text.size() && text[i] != '"'; ++i) {
      if (text[i] == '\\' && i + 1 < text.size()) { v += text[++i]; continue; }
      v += text[i];
    }
    out.push_back(v);
  }
  return out;
}

std::vector<std::wstring> steam_libraries() {
  std::vector<std::wstring> roots;
  std::wstring steam = reg_str(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath");
  if (steam.empty()) steam = reg_str(HKEY_LOCAL_MACHINE, L"SOFTWARE\\WOW6432Node\\Valve\\Steam", L"InstallPath");
  if (steam.empty()) steam = reg_str(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Valve\\Steam", L"InstallPath");
  if (steam.empty()) return roots;
  std::replace(steam.begin(), steam.end(), L'/', L'\\');
  roots.push_back(steam);
  std::string vdf = read_text(fs::path(steam) / L"steamapps" / L"libraryfolders.vdf");
  for (auto& p : vdf_values(vdf, "path")) {
    std::wstring w = widen(p);
    bool dup = false;
    for (auto& r : roots) dup |= _wcsicmp(r.c_str(), w.c_str()) == 0;
    if (!dup) roots.push_back(w);
  }
  return roots;
}

bool looks_like_game_dir(const std::wstring& dir) {
  std::error_code ec;
  return fs::exists(fs::path(dir) / kShippingExe, ec);
}

std::string read_build_id_for(const std::wstring& game_dir) {
  // ...\steamapps\common\<installdir>  ->  ...\steamapps\appmanifest_1032430.acf
  fs::path p(game_dir);
  fs::path acf = p.parent_path().parent_path() / (std::string("appmanifest_") + kSteamAppId + ".acf");
  auto ids = vdf_values(read_text(acf), "buildid");
  return ids.empty() ? std::string() : ids[0];
}

std::vector<GameInstall> find_game() {
  std::vector<GameInstall> found;
  for (auto& lib : steam_libraries()) {
    fs::path acf = fs::path(lib) / L"steamapps" / (std::string("appmanifest_") + kSteamAppId + ".acf");
    std::string text = read_text(acf);
    if (text.empty()) continue;
    auto dirs = vdf_values(text, "installdir");
    if (dirs.empty()) continue;
    GameInstall g;
    g.dir = (fs::path(lib) / L"steamapps" / L"common" / widen(dirs[0])).wstring();
    auto ids = vdf_values(text, "buildid");
    g.build_id = ids.empty() ? "" : ids[0];
    g.from_steam = true;
    if (looks_like_game_dir(g.dir)) found.push_back(g);
  }
  return found;
}

// ------------------------------------------------------------------ UE4SS detection
static std::string file_product_name(const fs::path& p) {
  DWORD h = 0, n = GetFileVersionInfoSizeW(p.c_str(), &h);
  if (!n) return {};
  std::vector<char> buf(n);
  if (!GetFileVersionInfoW(p.c_str(), 0, n, buf.data())) return {};
  struct LangCp { WORD lang, cp; } *tr = nullptr;
  UINT len = 0;
  if (!VerQueryValueW(buf.data(), L"\\VarFileInfo\\Translation", (void**)&tr, &len) || len < sizeof(LangCp)) return {};
  wchar_t q[128];
  swprintf_s(q, L"\\StringFileInfo\\%04x%04x\\ProductName", tr->lang, tr->cp);
  wchar_t* val = nullptr;
  if (!VerQueryValueW(buf.data(), q, (void**)&val, &len) || !val) return {};
  return narrow(val);
}

static bool is_ue4ss_proxy(const fs::path& p) {
  return file_product_name(p).find("UE4SS") != std::string::npos;
}

Ue4ssState detect_ue4ss(const std::wstring& game_dir, const Payload* payload) {
  Ue4ssState s;
  fs::path win64 = fs::path(game_dir) / kWin64;
  std::error_code ec;
  bool core = fs::exists(win64 / L"ue4ss" / L"UE4SS.dll", ec) || fs::exists(win64 / L"UE4SS.dll", ec);
  for (const wchar_t* proxy : {L"dwmapi.dll", L"xinput1_3.dll", L"dxgi.dll", L"d3d11.dll", L"version.dll", L"winmm.dll"}) {
    if (fs::exists(win64 / proxy, ec) && is_ue4ss_proxy(win64 / proxy)) {
      s.proxy = proxy;
      break;
    }
  }
  s.present = core && !s.proxy.empty();
  // version from the first lines of UE4SS.log, e.g. "UE4SS - v3.0.1 Beta #0 - Git SHA #83330635"
  std::string log = read_text(win64 / L"ue4ss" / L"UE4SS.log");
  size_t v = log.find("UE4SS - v");
  if (v != std::string::npos) s.version = log.substr(v + 8, log.find('\n', v) - (v + 8));
  if (!s.present && payload) {
    for (auto& f : payload->files) {
      if (f.component == 2 && f.rel_path.find(L'\\') == std::wstring::npos && fs::exists(win64 / f.rel_path, ec) &&
          !is_ue4ss_proxy(win64 / f.rel_path))
        s.proxy_conflict = true;
    }
  }
  return s;
}

// ------------------------------------------------------------------ manifest / status
static fs::path manifest_path(const std::wstring& game_dir) {
  return fs::path(game_dir) / kWin64 / kModRel / L"install-manifest.ini";
}

static fs::path appdata_dir() {
  wchar_t* p = nullptr;
  fs::path out;
  if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &p))) out = fs::path(p) / L"BloodTrailTogether";
  CoTaskMemFree(p);
  return out;
}

// Copy of the manifest outside the game folder, so UE4SS files we installed can still be found
// if the mod folder is deleted by hand.
static fs::path appdata_manifest(const std::wstring& game_dir) {
  size_t h = std::hash<std::wstring>{}(fs::path(game_dir).lexically_normal().wstring());
  wchar_t name[64];
  swprintf_s(name, L"install-%016zx.ini", h);
  return appdata_dir() / name;
}

InstallStatus read_install_status(const std::wstring& game_dir) {
  InstallStatus s;
  std::string text = read_text(manifest_path(game_dir));
  if (text.empty()) text = read_text(appdata_manifest(game_dir));
  if (text.empty()) return s;
  std::istringstream in(text);
  std::string line;
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.rfind("Version=", 0) == 0) s.version = line.substr(8);
    if (line == "UE4SSInstalledByUs=1") s.ue4ss_by_us = true;
  }
  s.installed = fs::exists(fs::path(game_dir) / kWin64 / kModRel / L"Scripts" / L"main.lua");
  return s;
}

bool game_running() {
  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snap == INVALID_HANDLE_VALUE) return false;
  PROCESSENTRY32W pe{sizeof(pe)};
  bool found = false;
  for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe))
    if (_wcsicmp(pe.szExeFile, L"BTVR-Win64-Shipping.exe") == 0) found = true;
  CloseHandle(snap);
  return found;
}

// ------------------------------------------------------------------ config merge
using Ini = std::vector<std::pair<std::string, std::map<std::string, std::string>>>;

static Ini parse_ini(const std::string& text) {
  Ini ini;
  std::istringstream in(text);
  std::string line, sec;
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.size() >= 3 && (unsigned char)line[0] == 0xEF) line = line.substr(3);
    if (line.empty() || line[0] == ';' || line[0] == '#') continue;
    if (line.front() == '[' && line.back() == ']') {
      sec = line.substr(1, line.size() - 2);
      ini.push_back({sec, {}});
      continue;
    }
    auto eq = line.find('=');
    if (eq == std::string::npos || ini.empty()) continue;
    ini.back().second[line.substr(0, eq)] = line.substr(eq + 1);
  }
  return ini;
}

// New template wins for release-controlled keys; the user's existing values win for the rest.
static std::string merge_config(const std::string& fresh, const std::string& existing) {
  if (existing.empty()) return fresh;
  static const std::set<std::string> forced = {"Relay.ProjectId", "Relay.Environment", "Updates.GitHubRepo"};
  Ini old = parse_ini(existing);
  std::map<std::string, std::string> oldkv;
  for (auto& [s, kv] : old)
    for (auto& [k, v] : kv) oldkv[s + "." + k] = v;
  // Rewrite the fresh template line by line so comments survive.
  std::istringstream in(fresh);
  std::ostringstream out;
  std::string line, sec;
  while (std::getline(in, line)) {
    std::string raw = line;
    if (!raw.empty() && raw.back() == '\r') raw.pop_back();
    if (!raw.empty() && raw.front() == '[' && raw.back() == ']') sec = raw.substr(1, raw.size() - 2);
    auto eq = raw.find('=');
    if (!raw.empty() && raw[0] != ';' && eq != std::string::npos && !sec.empty()) {
      std::string key = sec + "." + raw.substr(0, eq);
      auto it = oldkv.find(key);
      if (it != oldkv.end() && !forced.count(key)) raw = raw.substr(0, eq + 1) + it->second;
    }
    out << raw << "\r\n";
  }
  return out.str();
}

// ------------------------------------------------------------------ install / uninstall
static bool write_file(const fs::path& p, const unsigned char* d, size_t n, std::string& err) {
  std::error_code ec;
  fs::create_directories(p.parent_path(), ec);
  fs::path tmp = p;
  tmp += L".btt-new";
  {
    std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
    if (!f) { err = "cannot write " + narrow(p.wstring()) + " (is the folder read-only or the game running?)"; return false; }
    f.write((const char*)d, (std::streamsize)n);
    if (!f) { err = "write failed for " + narrow(p.wstring()) + " (disk full?)"; return false; }
  }
  if (!MoveFileExW(tmp.c_str(), p.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
    fs::remove(tmp, ec);
    err = "cannot replace " + narrow(p.wstring()) + " (file in use? close BloodTrail and try again)";
    return false;
  }
  return true;
}

static bool safe_rel(const std::wstring& rel) {
  if (rel.empty() || rel.find(L"..") != std::wstring::npos || rel.find(L':') != std::wstring::npos || rel[0] == L'\\')
    return false;
  std::wstring low = rel;
  std::transform(low.begin(), low.end(), low.begin(), ::towlower);
  if (low.size() >= 4 && low.substr(low.size() - 4) == L".exe") return false;  // never touch executables
  return true;
}

Result install(const std::wstring& game_dir, const Payload& p, const Progress& progress) {
  Result r;
  std::error_code ec;
  fs::path win64 = fs::path(game_dir) / kWin64;
  if (!looks_like_game_dir(game_dir)) {
    r.message = "This folder does not contain BloodTrail (BTVR-Win64-Shipping.exe not found).";
    return r;
  }
  if (game_running()) {
    r.message = "BloodTrail is running. Close the game, then click Install again.";
    return r;
  }
  for (auto& f : p.files)
    if (!safe_rel(f.rel_path)) {
      r.message = "Refusing unsafe payload path " + narrow(f.rel_path);
      return r;
    }

  progress(2, "Checking UE4SS...");
  Ue4ssState ue = detect_ue4ss(game_dir, &p);
  InstallStatus prev = read_install_status(game_dir);
  bool install_ue4ss = !ue.present;
  if (install_ue4ss && !p.has_ue4ss()) {
    r.message = "UE4SS is not installed and this installer does not include it.";
    return r;
  }
  if (install_ue4ss && ue.proxy_conflict) {
    r.message = "Another mod already uses the DLL name UE4SS needs (dwmapi.dll). Remove that mod or install UE4SS manually, then try again.";
    return r;
  }
  progress(5, ue.present ? "UE4SS found (" + (ue.version.empty() ? std::string("version unknown") : ue.version) + ") - keeping it"
                         : "UE4SS not found - it will be installed");

  // Back up the previous BloodTrail Together folder before replacing it.
  fs::path mod = win64 / kModRel;
  std::string old_config = read_text(mod / L"config.ini");
  if (fs::exists(mod, ec)) {
    char stamp[32];
    time_t t = time(nullptr);
    tm lt;
    localtime_s(&lt, &t);
    strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &lt);
    fs::path backup = appdata_dir() / L"backups" / widen(stamp);
    fs::create_directories(backup, ec);
    fs::copy(mod, backup / L"BloodTrailTogether", fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
    progress(8, ec ? "Warning: backup failed: " + ec.message() : "Backed up previous version to " + narrow(backup.wstring()));
    // Remove old program files (not logs/config) so renamed files from older versions do not linger.
    fs::remove_all(mod / L"Scripts", ec);
    fs::remove_all(mod / L"bin", ec);
  }

  std::vector<std::wstring> written;
  size_t total = 0, done = 0;
  for (auto& f : p.files)
    if (f.component == 1 || install_ue4ss) total += f.data.size();
  for (auto& f : p.files) {
    if (f.component == 2 && !install_ue4ss) continue;
    fs::path dst = win64 / f.rel_path;
    std::string err;
    bool is_config = f.component == 1 && _wcsicmp(dst.filename().c_str(), L"config.ini") == 0;
    if (is_config) {
      std::string merged = merge_config(std::string(f.data.begin(), f.data.end()), old_config);
      if (!write_file(dst, (const unsigned char*)merged.data(), merged.size(), err)) { r.message = err; return r; }
    } else if (f.component == 2 && fs::exists(dst, ec)) {
      // UE4SS file already exists (partial install): keep the user's copy of settings/mod lists.
      std::wstring name = dst.filename().wstring();
      if (_wcsicmp(name.c_str(), L"UE4SS-settings.ini") == 0 || _wcsicmp(name.c_str(), L"mods.txt") == 0 ||
          _wcsicmp(name.c_str(), L"mods.json") == 0)
        continue;
      if (!write_file(dst, f.data.data(), f.data.size(), err)) { r.message = err; return r; }
    } else {
      if (!write_file(dst, f.data.data(), f.data.size(), err)) { r.message = err; return r; }
    }
    written.push_back(f.rel_path);
    done += f.data.size();
    progress(10 + (int)(85.0 * done / std::max<size_t>(total, 1)), "Installed " + narrow(f.rel_path));
  }

  // Manifest (in the mod folder and a copy in %LOCALAPPDATA%).
  std::ostringstream m;
  char stamp[32];
  time_t t = time(nullptr);
  tm lt;
  localtime_s(&lt, &t);
  strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", &lt);
  m << "[Install]\r\nVersion=" << p.version() << "\r\nDate=" << stamp << "\r\nUE4SSInstalledByUs="
    << ((install_ue4ss || prev.ue4ss_by_us) ? 1 : 0) << "\r\nGameDir=" << narrow(game_dir) << "\r\n[Files]\r\n";
  for (auto& w : written) m << "File=" << narrow(w) << "\r\n";
  std::string ms = m.str();
  std::string err;
  if (!write_file(manifest_path(game_dir), (const unsigned char*)ms.data(), ms.size(), err)) { r.message = err; return r; }
  fs::create_directories(appdata_dir(), ec);
  write_file(appdata_manifest(game_dir), (const unsigned char*)ms.data(), ms.size(), err);

  progress(100, "BloodTrail Together " + p.version() + " installed.");
  r.ok = true;
  r.message = "Installed. Start BloodTrail through Steam and press F9 in game for the multiplayer window.";
  return r;
}

Result uninstall(const std::wstring& game_dir, bool remove_ue4ss, const Progress& progress) {
  Result r;
  std::error_code ec;
  if (game_running()) {
    r.message = "BloodTrail is running. Close the game first.";
    return r;
  }
  fs::path win64 = fs::path(game_dir) / kWin64;
  std::string text = read_text(manifest_path(game_dir));
  if (text.empty()) text = read_text(appdata_manifest(game_dir));
  bool ue4ss_by_us = text.find("UE4SSInstalledByUs=1") != std::string::npos;
  progress(10, "Removing BloodTrail Together...");
  fs::remove_all(win64 / kModRel, ec);
  if (ec) {
    r.message = "Could not remove the mod folder: " + ec.message();
    return r;
  }
  if (remove_ue4ss) {
    if (!ue4ss_by_us) {
      progress(50, "UE4SS was not installed by BloodTrail Together - leaving it in place.");
    } else {
      // Only remove UE4SS files we wrote, and only if no other mods depend on it.
      bool others = false;
      for (auto& e : fs::directory_iterator(win64 / L"ue4ss" / L"Mods", ec)) {
        std::wstring n = e.path().filename().wstring();
        static const std::set<std::wstring> stock = {L"shared", L"BPModLoaderMod", L"BPML_GenericFunctions", L"CheatManagerEnablerMod",
                                                     L"ConsoleCommandsMod", L"ConsoleEnablerMod", L"Keybinds", L"LineTraceMod",
                                                     L"SplitScreenMod", L"jsbLuaProfilerMod", L"mods.txt", L"mods.json"};
        if (e.is_directory() && !stock.count(n)) others = true;
      }
      if (others) {
        progress(50, "Other UE4SS mods are installed - keeping UE4SS so they keep working.");
      } else {
        std::istringstream in(text);
        std::string line;
        while (std::getline(in, line)) {
          if (!line.empty() && line.back() == '\r') line.pop_back();
          if (line.rfind("File=", 0) != 0) continue;
          std::wstring rel = widen(line.substr(5));
          if (!safe_rel(rel)) continue;
          fs::remove(win64 / rel, ec);
        }
        if (is_ue4ss_proxy(win64 / L"dwmapi.dll")) fs::remove(win64 / L"dwmapi.dll", ec);
        fs::remove_all(win64 / L"ue4ss", ec);
        progress(80, "UE4SS removed.");
      }
    }
  }
  fs::remove(appdata_manifest(game_dir), ec);
  progress(100, "BloodTrail Together removed.");
  r.ok = true;
  r.message = "BloodTrail Together has been removed. Your game files were not modified.";
  return r;
}

}  // namespace bttinst
