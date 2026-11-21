#pragma once
#include <map>
#include <string>

namespace btt {

// Minimal INI reader/writer ([Section] key=value). Keys are case-sensitive "Section.Key".
class Config {
 public:
  bool load(const std::wstring& path);
  bool save(const std::wstring& path) const;
  std::string get(const std::string& key, const std::string& def = "") const;
  int get_int(const std::string& key, int def) const;
  double get_double(const std::string& key, double def) const;
  bool get_bool(const std::string& key, bool def) const;
  void set(const std::string& key, const std::string& value);

 private:
  std::map<std::string, std::string> kv_;
};

// All runtime settings, resolved from config.ini (+ optional dev.ini overrides).
struct Settings {
  // Unity Relay: a PUBLIC project id + environment. No secrets.
  std::string unity_project_id;
  std::string unity_environment = "production";
  std::string relay_region;  // empty = let Unity pick (QoS)
  std::string player_name = "Player";
  // Sync tuning
  int pose_send_hz = 30;
  int interp_delay_ms = 100;
  int max_extrapolate_ms = 150;
  // Audio sharing: capture sounds within this radius (cm) of the local player's head/hands.
  double share_sound_radius_cm = 180.0;
  // Dev-only
  bool dev_mode = false;
  int sim_latency_ms = 0;
  int sim_jitter_ms = 0;
  double sim_loss_pct = 0.0;
  bool packet_log = false;
  // Update checks against GitHub Releases ("owner/repo", empty disables)
  std::string update_repo;
  bool update_check = true;

  static Settings from(const Config& c);
};

}  // namespace btt
