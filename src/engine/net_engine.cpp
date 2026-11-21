#include "engine/net_engine.h"

#include <windows.h>
#include <mmsystem.h>

#include <cmath>

#include "core/log.h"
#include "core/util.h"
#include "net/loopback_transport.h"
#include "net/udp_transport.h"
#include "relay/relay_transport.h"

namespace btt {

namespace {

// Rotate v around Z (yaw) by `yaw` radians.
Vec3 rotz(Vec3 v, float yaw) {
  float c = std::cos(yaw), s = std::sin(yaw);
  return {v.x * c - v.y * s, v.x * s + v.y * c, v.z};
}
Quat qmul(Quat a, Quat b) {
  return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
          a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}
Quat yaw_quat(float yaw) { return {0, 0, std::sin(yaw / 2), std::cos(yaw / 2)}; }
float yaw_of(Quat q) {  // forward vector X rotated by q, projected on XY
  float fx = 1 - 2 * (q.y * q.y + q.z * q.z);
  float fy = 2 * (q.x * q.y + q.w * q.z);
  return std::atan2(fy, fx);
}

std::string map_short(const std::string& path) {
  auto p = path.find_last_of('/');
  return p == std::string::npos ? path : path.substr(p + 1);
}

}  // namespace

NetEngine::NetEngine(const Settings& s) : settings_(s) {
  imp_.latency_ms = s.sim_latency_ms;
  imp_.jitter_ms = s.sim_jitter_ms;
  imp_.loss_pct = s.sim_loss_pct;
}

NetEngine::~NetEngine() { stop(); }

void NetEngine::start() {
  if (run_.exchange(true)) return;
  net_startup();
  thread_ = std::thread([this] { loop(); });
}

void NetEngine::stop() {
  if (!run_.exchange(false)) return;
  if (thread_.joinable()) thread_.join();
  std::lock_guard<std::mutex> lk(mu_);
  end_locked("shutdown");
  net_cleanup();
}

void NetEngine::loop() {
  timeBeginPeriod(1);
  while (run_) {
    double now = now_sec();
    {
      std::lock_guard<std::mutex> lk(mu_);
      tick(now);
    }
    Sleep(2);
  }
  timeEndPeriod(1);
}

void NetEngine::begin(TransportPtr t, Role role, bool simulated) {
  end_locked("");
  auto imp = std::make_unique<ImpairedTransport>(std::move(t));
  imp->set_params(imp_);
  impaired_ = imp.get();
  session_ = std::make_unique<Session>(std::move(imp), role, settings_.player_name, BTT_VERSION);
  session_->set_pose_rate(settings_.pose_send_hz);
  session_->configure_interp(settings_.interp_delay_ms / 1000.0, settings_.max_extrapolate_ms / 1000.0);
  if (have_local_pose_) session_->set_local_pose(local_pose_);
  simulated_ = simulated;
  last_error_.clear();
  remote_map_.clear();
  announced_map_.clear();
  last_state_ = SessionState::Idle;
}

void NetEngine::end_locked(const std::string& reason) {
  if (session_) {
    session_->close();
    session_.reset();
    impaired_ = nullptr;
    if (!reason.empty()) BTT_INFO("engine: session ended (%s)", reason.c_str());
  }
  if (bot_) {
    bot_->close();
    bot_.reset();
  }
  bot_echo_.clear();
  simulated_ = false;
  remote_map_.clear();
}

void NetEngine::host_relay() {
  std::lock_guard<std::mutex> lk(mu_);
  BTT_INFO("engine: creating Unity Relay session");
  begin(RelayTransport::host({settings_.unity_project_id, settings_.unity_environment, settings_.relay_region}),
        Role::Host, false);
}

void NetEngine::join_relay(const std::string& code) {
  std::lock_guard<std::mutex> lk(mu_);
  BTT_INFO("engine: joining Unity Relay session %s", code.c_str());
  begin(RelayTransport::join({settings_.unity_project_id, settings_.unity_environment, settings_.relay_region}, code),
        Role::Client, false);
}

void NetEngine::host_dev_udp(uint16_t port) {
  std::lock_guard<std::mutex> lk(mu_);
  BTT_INFO("engine: DEV udp host on port %u", port);
  begin(UdpTransport::host(port), Role::Host, false);
}

void NetEngine::join_dev_udp(const std::string& ip, uint16_t port) {
  std::lock_guard<std::mutex> lk(mu_);
  BTT_INFO("engine: DEV udp join %s:%u", ip.c_str(), port);
  begin(UdpTransport::join(ip, port), Role::Client, false);
}

void NetEngine::start_simulated_peer(SimMode mode) {
  std::lock_guard<std::mutex> lk(mu_);
  auto [a, b] = LoopbackTransport::make_pair();
  begin(std::move(a), Role::Host, true);
  bot_ = std::make_unique<Session>(std::move(b), Role::Client, "SimBot", BTT_VERSION);
  bot_->set_pose_rate(settings_.pose_send_hz);
  bot_->configure_interp(settings_.interp_delay_ms / 1000.0, settings_.max_extrapolate_ms / 1000.0);
  sim_mode_ = mode;
  bot_anchor_set_ = false;
  bot_start_ = now_sec();
  bot_map_.clear();
  BTT_INFO("engine: simulated player started (%s)", mode == SimMode::Mirror ? "mirror" : "orbit");
}

void NetEngine::disconnect() {
  std::lock_guard<std::mutex> lk(mu_);
  end_locked("user disconnected");
  last_error_.clear();
}

void NetEngine::set_local_pose(const PoseFrame& f) {
  std::lock_guard<std::mutex> lk(mu_);
  local_pose_ = f;
  have_local_pose_ = true;
  if (session_) session_->set_local_pose(f);
}

void NetEngine::set_local_map(const std::string& map_path) {
  std::lock_guard<std::mutex> lk(mu_);
  if (map_path == local_map_) return;
  BTT_INFO("engine: local map is now %s", map_path.c_str());
  local_map_ = map_path;
}

void NetEngine::report_sound(const std::string& asset, Vec3 pos, float vol, float pitch) {
  if (!share_sounds_) return;
  std::lock_guard<std::mutex> lk(mu_);
  if (!session_ || session_->state() != SessionState::Connected) return;
  NetEvent e;
  e.type = EventType::Sound;
  e.s1 = asset;
  e.v = pos;
  e.f0 = vol;
  e.f1 = pitch;
  session_->send_event(std::move(e));
}

void NetEngine::send_notice(const std::string& text) {
  std::lock_guard<std::mutex> lk(mu_);
  if (!session_) return;
  NetEvent e;
  e.type = EventType::Notice;
  e.s1 = text;
  session_->send_event(std::move(e));
}

void NetEngine::send_world(const std::string& key, const std::string& value) {
  std::lock_guard<std::mutex> lk(mu_);
  if (!session_ || session_->role() != Role::Host) return;  // world state is host-authoritative
  NetEvent e;
  e.type = EventType::World;
  e.s1 = key;
  e.s2 = value;
  session_->send_event(std::move(e));
}

void NetEngine::send_game(const std::string& line) {
  std::lock_guard<std::mutex> lk(mu_);
  if (!session_ || session_->state() != SessionState::Connected) return;
  NetEvent e;
  e.type = EventType::Game;
  e.s1 = line;
  session_->send_event(std::move(e));
}

bool NetEngine::remote_pose(PoseFrame& out) {
  std::lock_guard<std::mutex> lk(mu_);
  if (!session_) return false;
  if (!remote_map_.empty() && !local_map_.empty() && remote_map_ != local_map_) return false;
  return session_->sample_remote(now_sec(), out);
}

void NetEngine::push_cmd(GameCommand c) {
  if (cmds_.size() > 256) cmds_.erase(cmds_.begin());
  cmds_.push_back(std::move(c));
}

void NetEngine::pop_commands(std::vector<GameCommand>& out) {
  std::lock_guard<std::mutex> lk(mu_);
  out.insert(out.end(), cmds_.begin(), cmds_.end());
  cmds_.clear();
}

void NetEngine::handle_event(const NetEvent& e, double now) {
  switch (e.type) {
    case EventType::Map:
      remote_map_ = e.s1;
      BTT_INFO("engine: other player is on %s", e.s1.c_str());
      break;
    case EventType::Sound:
      // Late sounds are worse than missing ones (they no longer match what you see).
      if (e.age_s < 0.75) push_cmd({GameCommand::Sound, e.s1, e.v, e.f0, e.f1});
      break;
    case EventType::Notice:
      push_cmd({GameCommand::Notify, e.s1});
      break;
    case EventType::World:
      push_cmd({GameCommand::Ui, "world " + e.s1 + " " + e.s2});
      break;
    case EventType::Request:
      break;
    case EventType::Game:
      push_cmd({GameCommand::Ui, "game " + e.s1});
      break;
  }
  (void)now;
}

void NetEngine::tick(double now) {
  if (!session_) return;
  session_->update(now);
  if (bot_) sim_tick(now);

  SessionState st = session_->state();
  if (st != last_state_) {
    if (st == SessionState::Connected && last_state_ != SessionState::Interrupted) {
      push_cmd({GameCommand::Notify, session_->peer_name() + " joined"});
      announced_map_.clear();  // re-announce our map to the new peer
    } else if (st == SessionState::Interrupted) {
      push_cmd({GameCommand::Notify, "Connection interrupted..."});
    } else if (st == SessionState::Connected && last_state_ == SessionState::Interrupted) {
      push_cmd({GameCommand::Notify, "Connection restored"});
    } else if (st == SessionState::Disconnected) {
      std::string why = session_->error();
      push_cmd({GameCommand::Notify, "Disconnected" + (why.empty() ? "" : ": " + why)});
    }
    last_state_ = st;
  }
  if (st == SessionState::Disconnected) {
    last_error_ = session_->error();
    return;
  }

  std::vector<NetEvent> evs;
  session_->pop_events(evs);
  for (auto& e : evs) handle_event(e, now);

  if (st == SessionState::Connected) {
    if (!local_map_.empty() && local_map_ != announced_map_) {
      NetEvent e;
      e.type = EventType::Map;
      e.s1 = local_map_;
      session_->send_event(e);
      announced_map_ = local_map_;
    }
    // Client follows the host's map (both game modes load maps with OpenLevel).
    if (session_->role() == Role::Client && !remote_map_.empty() && !local_map_.empty() && remote_map_ != local_map_) {
      if (last_travel_target_ != remote_map_ || now - last_travel_req_ > 15.0) {
        BTT_INFO("engine: following host to %s", remote_map_.c_str());
        push_cmd({GameCommand::Travel, remote_map_});
        last_travel_target_ = remote_map_;
        last_travel_req_ = now;
      }
    }
  }
}

// In-process simulated second player. It runs a real Session over a loopback transport, so the
// whole pipeline (encode -> impairment -> reliable/unreliable channels -> interpolation) is
// exercised. It plays the CLIENT role while the local game is the HOST.
void NetEngine::sim_tick(double now) {
  bot_->update(now);
  if (bot_->state() == SessionState::Disconnected) return;

  // What does the bot "see" of the real player? Use the networked (interpolated) pose.
  PoseFrame seen;
  bool have_seen = bot_->sample_remote(now, seen);
  if (have_seen && !bot_anchor_set_) {
    // Anchor 150 cm in front of where the player was looking when the bot spawned.
    float yaw = yaw_of(seen.head.q);
    bot_anchor_.p = {seen.head.p.x + std::cos(yaw) * 150.0f, seen.head.p.y + std::sin(yaw) * 150.0f, seen.head.p.z};
    bot_anchor_.q = yaw_quat(yaw);
    bot_anchor_set_ = true;
  }

  PoseFrame f{};
  f.flags = 3;
  if (bot_anchor_set_ && have_seen) {
    if (sim_mode_ == SimMode::Mirror) {
      // Copy the player's motion, rotated 180 deg about a vertical axis halfway between them,
      // so the bot faces you and copies your gestures.
      float yaw = yaw_of(bot_anchor_.q);
      Vec3 pivot = {bot_anchor_.p.x - std::cos(yaw) * 75.0f, bot_anchor_.p.y - std::sin(yaw) * 75.0f, 0};
      auto tf = [&](const Xform& x) {
        Vec3 d = {x.p.x - pivot.x, x.p.y - pivot.y, x.p.z};
        Vec3 r = rotz(d, 3.14159265f);
        return Xform{{r.x + pivot.x, r.y + pivot.y, r.z}, qmul(yaw_quat(3.14159265f), x.q)};
      };
      f.head = tf(seen.head);
      f.left = tf(seen.right);
      f.right = tf(seen.left);
      f.body = tf(seen.body);
    } else {
      // Orbit: walk a 2 m circle around the anchor, look at the centre, wave hands.
      double t = now - bot_start_;
      float ang = (float)(t * 0.4);
      Vec3 c = bot_anchor_.p;
      Vec3 head = {c.x + std::cos(ang) * 200.0f, c.y + std::sin(ang) * 200.0f, c.z};
      float face = ang + 3.14159265f;
      Quat hq = yaw_quat(face);
      auto hand = [&](float side, float phase) {
        Vec3 off = rotz({35.0f, side * 25.0f, -45.0f + 20.0f * (float)std::sin(t * 3.0 + phase)}, face);
        return Xform{{head.x + off.x, head.y + off.y, head.z + off.z}, hq};
      };
      f.head = {head, hq};
      f.body = {{head.x, head.y, c.z - 165.0f}, hq};  // floor under the bot
      f.left = hand(-1, 0);
      f.right = hand(1, 1.5f);
    }
    bot_->set_local_pose(f);
  }

  // Bot follows the host's map and echoes the host's sounds back from its own head position,
  // so shared audio can be heard/tested without a second person.
  std::vector<NetEvent> evs;
  bot_->pop_events(evs);
  for (auto& e : evs) {
    if (e.type == EventType::Map && e.s1 != bot_map_) {
      bot_map_ = e.s1;
      NetEvent m;
      m.type = EventType::Map;
      m.s1 = bot_map_;
      bot_->send_event(m);
    } else if (e.type == EventType::Sound) {
      NetEvent echo = e;
      echo.t_ms = 0;
      if (bot_anchor_set_) echo.v = f.head.p;
      bot_echo_.push_back({now + 0.75, echo});
    }
  }
  while (!bot_echo_.empty() && bot_echo_.front().due <= now) {
    bot_->send_event(bot_echo_.front().ev);
    bot_echo_.pop_front();
  }
}

EngineSnapshot NetEngine::snapshot() {
  std::lock_guard<std::mutex> lk(mu_);
  EngineSnapshot s;
  s.local_map = map_short(local_map_);
  if (!session_) {
    s.state = "Idle";
    s.error = last_error_;
    return s;
  }
  double now = now_sec();
  SessionState st = session_->state();
  LinkState ls = session_->link_state();
  s.role = session_->role() == Role::Host ? "host" : "client";
  s.transport = session_->transport()->name();
  s.simulated = simulated_;
  s.join_code = session_->join_code();
  s.error = session_->error();
  s.peer_name = session_->peer_name();
  s.peer_version = session_->peer_version();
  s.remote_map = map_short(remote_map_);
  s.same_map = !remote_map_.empty() && remote_map_ == local_map_;
  switch (st) {
    case SessionState::Connected: s.state = "Connected"; break;
    case SessionState::Interrupted: s.state = "Interrupted"; break;
    case SessionState::Disconnected: s.state = "Disconnected"; break;
    case SessionState::Handshaking: s.state = "Connecting"; break;
    default:
      s.state = ls == LinkState::WaitingForPeer ? "Waiting" : ls == LinkState::Connecting ? "Connecting" : "Preparing";
  }
  auto stats = session_->stats(now);
  s.rtt_ms = stats.rtt_ms;
  s.loss_pct = stats.loss_pct;
  s.jitter_ms = stats.jitter_ms;
  s.bytes_in = stats.bytes_in;
  s.bytes_out = stats.bytes_out;
  s.pkts_in = stats.pkts_in;
  s.pkts_out = stats.pkts_out;
  if (st == SessionState::Connected)
    s.quality = (s.rtt_ms < 120 && s.loss_pct < 3) ? 3 : (s.rtt_ms < 250 && s.loss_pct < 10) ? 2 : 1;
  return s;
}

void NetEngine::set_impairment(const ImpairmentParams& p) {
  std::lock_guard<std::mutex> lk(mu_);
  imp_ = p;
  if (impaired_) impaired_->set_params(p);
}

ImpairmentParams NetEngine::impairment() {
  std::lock_guard<std::mutex> lk(mu_);
  return imp_;
}

Settings NetEngine::settings() {
  std::lock_guard<std::mutex> lk(mu_);
  return settings_;
}

void NetEngine::set_player_name(const std::string& n) {
  std::lock_guard<std::mutex> lk(mu_);
  settings_.player_name = n.empty() ? "Player" : n;
}

}  // namespace btt
