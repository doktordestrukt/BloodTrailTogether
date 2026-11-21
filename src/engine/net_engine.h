#pragma once
#include <atomic>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "core/config.h"
#include "net/impairment.h"
#include "net/session.h"

namespace btt {

enum class SimMode { Mirror, Orbit };

// What the UI / game shows. Copied out under a lock, safe to read anywhere.
struct EngineSnapshot {
  std::string state = "Idle";  // Idle, Preparing, Waiting, Connecting, Connected, Interrupted, Disconnected
  std::string role;            // host / client / ""
  std::string transport;       // unity-relay / udp-dev / loopback
  std::string join_code;
  std::string error;
  std::string peer_name;
  std::string peer_version;
  std::string local_map, remote_map;
  bool simulated = false;
  bool same_map = false;
  double rtt_ms = 0, loss_pct = 0, jitter_ms = 0;
  uint64_t bytes_in = 0, bytes_out = 0, pkts_in = 0, pkts_out = 0;
  int quality = 0;  // 0 none, 1 bad, 2 ok, 3 good
};

// Instruction for the game side (Lua), produced by the engine.
struct GameCommand {
  enum Kind { Travel, Sound, Notify, Ui } kind;
  std::string a;
  Vec3 v;
  float f0 = 1, f1 = 1;
};

// Owns the session and the network thread. All public methods are thread-safe.
class NetEngine {
 public:
  explicit NetEngine(const Settings& s);
  ~NetEngine();

  void start();
  void stop();

  // Session control (from UI thread or Lua bridge)
  void host_relay();
  void join_relay(const std::string& code);
  void host_dev_udp(uint16_t port);
  void join_dev_udp(const std::string& ip, uint16_t port);
  void start_simulated_peer(SimMode mode);
  void disconnect();

  // Game -> engine (called each game frame from the Lua bridge)
  void set_local_pose(const PoseFrame& f);
  void set_local_map(const std::string& map_path);
  void report_sound(const std::string& asset, Vec3 pos, float vol, float pitch);
  void send_notice(const std::string& text);
  void send_world(const std::string& key, const std::string& value);
  void send_game(const std::string& line);  // reliable, opaque to the engine
  void notify_local(const std::string& text) { push_cmd({GameCommand::Notify, text}); }  // note in the headset

  // Engine -> game
  bool remote_pose(PoseFrame& out);
  void pop_commands(std::vector<GameCommand>& out);
  EngineSnapshot snapshot();

  void set_impairment(const ImpairmentParams& p);
  ImpairmentParams impairment();
  Settings settings();
  void set_player_name(const std::string& n);
  void set_share_sounds(bool on) { share_sounds_ = on; }
  bool share_sounds() const { return share_sounds_; }

 private:
  void loop();
  void begin(TransportPtr t, Role role, bool simulated);
  void end_locked(const std::string& reason);
  void tick(double now);
  void sim_tick(double now);
  void handle_event(const NetEvent& e, double now);
  void push_cmd(GameCommand c);

  Settings settings_;
  std::mutex mu_;
  std::thread thread_;
  std::atomic<bool> run_{false};
  std::atomic<bool> share_sounds_{true};

  std::unique_ptr<Session> session_;
  ImpairedTransport* impaired_ = nullptr;  // owned by session_
  ImpairmentParams imp_{};
  bool simulated_ = false;
  std::string last_error_;

  // simulated peer (in-process bot over loopback)
  std::unique_ptr<Session> bot_;
  SimMode sim_mode_ = SimMode::Mirror;
  bool bot_anchor_set_ = false;
  Xform bot_anchor_{};
  double bot_start_ = 0;
  struct Delayed { double due; NetEvent ev; };
  std::deque<Delayed> bot_echo_;
  std::string bot_map_;

  // game state mirrors
  PoseFrame local_pose_{};
  bool have_local_pose_ = false;
  std::string local_map_, remote_map_, announced_map_;
  double last_travel_req_ = -100;
  std::string last_travel_target_;
  SessionState last_state_ = SessionState::Idle;
  std::vector<GameCommand> cmds_;
};

}  // namespace btt
