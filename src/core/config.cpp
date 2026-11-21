#include "core/config.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace btt {

static std::string trim(const std::string& s) {
  size_t a = s.find_first_not_of(" \t\r\n");
  if (a == std::string::npos) return {};
  size_t b = s.find_last_not_of(" \t\r\n");
  return s.substr(a, b - a + 1);
}

bool Config::load(const std::wstring& path) {
  std::ifstream f(path);
  if (!f) return false;
  std::string line, section;
  bool first = true;
  while (std::getline(f, line)) {
    if (first && line.size() >= 3 && (unsigned char)line[0] == 0xEF && (unsigned char)line[1] == 0xBB &&
        (unsigned char)line[2] == 0xBF)
      line = line.substr(3);  // UTF-8 BOM (Notepad / PowerShell)
    first = false;
    line = trim(line);
    if (line.empty() || line[0] == ';' || line[0] == '#') continue;
    if (line.front() == '[' && line.back() == ']') {
      section = trim(line.substr(1, line.size() - 2));
      continue;
    }
    auto eq = line.find('=');
    if (eq == std::string::npos) continue;
    kv_[section + "." + trim(line.substr(0, eq))] = trim(line.substr(eq + 1));
  }
  return true;
}

bool Config::save(const std::wstring& path) const {
  std::map<std::string, std::map<std::string, std::string>> sections;
  for (auto& [k, v] : kv_) {
    auto dot = k.find('.');
    sections[k.substr(0, dot)][k.substr(dot + 1)] = v;
  }
  std::ofstream f(path, std::ios::trunc);
  if (!f) return false;
  for (auto& [s, kv] : sections) {
    f << "[" << s << "]\n";
    for (auto& [k, v] : kv) f << k << "=" << v << "\n";
    f << "\n";
  }
  return true;
}

std::string Config::get(const std::string& key, const std::string& def) const {
  auto it = kv_.find(key);
  return it == kv_.end() ? def : it->second;
}
int Config::get_int(const std::string& key, int def) const {
  auto v = get(key);
  return v.empty() ? def : atoi(v.c_str());
}
double Config::get_double(const std::string& key, double def) const {
  auto v = get(key);
  return v.empty() ? def : atof(v.c_str());
}
bool Config::get_bool(const std::string& key, bool def) const {
  auto v = get(key);
  if (v.empty()) return def;
  return v == "1" || v == "true" || v == "True" || v == "yes";
}
void Config::set(const std::string& key, const std::string& value) { kv_[key] = value; }

Settings Settings::from(const Config& c) {
  Settings s;
  s.unity_project_id = c.get("Relay.ProjectId");
  s.unity_environment = c.get("Relay.Environment", "production");
  s.relay_region = c.get("Relay.Region");
  s.player_name = c.get("Player.Name", "Player");
  s.pose_send_hz = c.get_int("Sync.PoseSendHz", 30);
  s.interp_delay_ms = c.get_int("Sync.InterpDelayMs", 100);
  s.max_extrapolate_ms = c.get_int("Sync.MaxExtrapolateMs", 150);
  s.share_sound_radius_cm = c.get_double("Audio.ShareRadiusCm", 180.0);
  s.dev_mode = c.get_bool("Dev.Enabled", false);
  s.sim_latency_ms = c.get_int("Dev.SimLatencyMs", 0);
  s.sim_jitter_ms = c.get_int("Dev.SimJitterMs", 0);
  s.sim_loss_pct = c.get_double("Dev.SimLossPct", 0.0);
  s.packet_log = c.get_bool("Dev.PacketLog", false);
  s.update_repo = c.get("Updates.GitHubRepo");
  s.update_check = c.get_bool("Updates.Check", true);
  if (s.pose_send_hz < 5) s.pose_send_hz = 5;
  if (s.pose_send_hz > 90) s.pose_send_hz = 90;
  return s;
}

}  // namespace btt
