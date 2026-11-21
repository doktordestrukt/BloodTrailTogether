#pragma once
#include <cstdint>
#include <deque>
#include <map>
#include <string>
#include <vector>

#include "net/transport.h"
#include "sync/interp.h"
#include "sync/pose.h"

namespace btt {

constexpr uint16_t kProtocolVersion = 2;  // 2: body transform in poses, Game events

enum class Role : uint8_t { Host = 1, Client = 2 };

enum class SessionState { Idle, Linking, Handshaking, Connected, Interrupted, Disconnected };
const char* session_state_name(SessionState s);

// Gameplay-level event carried on the reliable channel.
enum class EventType : uint8_t {
  Map = 1,      // s1 = map package path ("/Game/Maps/.../Map_X"), f0 = epoch
  Sound = 2,    // s1 = sound asset path, v = world location, f0 = volume, f1 = pitch
  Notice = 3,   // s1 = text shown to the other player
  World = 4,    // s1 = key (actor/system id), s2 = value  (host-authoritative world state)
  Request = 5,  // s1 = request name, s2 = argument (client -> host)
  Game = 6,     // s1 = opaque game-script event line (hits, deaths, NPC sync); interpreted by main.lua
};

struct NetEvent {
  EventType type = EventType::Notice;
  uint32_t t_ms = 0;  // sender clock when created
  std::string s1, s2;
  Vec3 v;
  float f0 = 0, f1 = 0;
  double age_s = 0;  // filled on receive: estimated age since the sender created it
};

struct SessionStats {
  double rtt_ms = 0;
  double loss_pct = 0;
  double jitter_ms = 0;
  uint64_t pkts_out = 0, pkts_in = 0, bytes_out = 0, bytes_in = 0;
  size_t reliable_pending = 0;
  double since_last_rx = 0;
};

// Two-peer session over any ITransport:
//  - unreliable, latest-wins pose stream (interpolated on the receiver)
//  - reliable, ordered event stream (selective resend, cumulative ack in every packet)
//  - handshake with protocol version check, RTT pings, loss estimate, timeouts,
//    automatic resync when the peer restarts (new session id).
class Session {
 public:
  Session(TransportPtr transport, Role role, std::string local_name, std::string mod_version);
  ~Session();

  void set_pose_rate(int hz) { pose_interval_ = 1.0 / hz; }
  void configure_interp(double delay_s, double max_extrap_s) { interp_.configure(delay_s, max_extrap_s); }

  void update(double now);
  void set_local_pose(const PoseFrame& f);
  void send_event(NetEvent ev);
  void pop_events(std::vector<NetEvent>& out);
  bool sample_remote(double now, PoseFrame& out) const;

  SessionState state() const { return state_; }
  Role role() const { return role_; }
  const std::string& peer_name() const { return peer_name_; }
  const std::string& peer_version() const { return peer_version_; }
  std::string error() const;
  std::string join_code() const { return transport_->join_code(); }
  LinkState link_state() const { return transport_->state(); }
  SessionStats stats(double now) const;
  ITransport* transport() { return transport_.get(); }
  void close();

  // Packet tap for the inspector / packet log: dir 0=out 1=in.
  using Tap = void (*)(void* ctx, int dir, const uint8_t* d, size_t n);
  void set_tap(Tap t, void* ctx) { tap_ = t; tap_ctx_ = ctx; }

 private:
  struct OutRel {
    uint16_t id;
    std::vector<uint8_t> body;
    double last_sent = -1;
    int sends = 0;
  };
  void handle_packet(const uint8_t* d, size_t n, double now);
  void handle_rel(uint16_t id, const uint8_t* d, size_t n, double now);
  void deliver_rel(const std::vector<uint8_t>& body, double now);
  void queue_rel(std::vector<uint8_t> body);
  void send_hello(bool welcome);
  void flush(double now, bool force);
  void reset_peer();
  uint32_t clock_ms() const { return (uint32_t)(uint64_t)(clock_ * 1000.0); }
  double clock_ = 0;  // last time passed to update(); all wire timestamps use it

  TransportPtr transport_;
  Role role_;
  std::string local_name_, mod_version_;
  std::string peer_name_, peer_version_, error_;
  SessionState state_ = SessionState::Linking;
  uint32_t sid_;
  uint32_t peer_sid_ = 0;
  uint16_t seq_ = 0;

  // reliable out
  uint16_t rel_next_ = 0;
  std::deque<OutRel> rel_out_;
  // reliable in
  uint16_t rel_expected_ = 0;
  std::map<uint16_t, std::vector<uint8_t>> rel_buf_;
  std::vector<NetEvent> inbox_;

  // pose
  PoseFrame local_pose_{};
  bool have_local_pose_ = false;
  double pose_interval_ = 1.0 / 30;
  double last_pose_sent_ = -1;
  PoseInterpolator interp_;

  // timing / stats
  double last_rx_ = -1, last_tx_ = -1, last_ping_ = -1, hello_sent_ = -1;
  double rtt_ = 0;
  bool rtt_valid_ = false;
  uint64_t pkts_out_ = 0, pkts_in_ = 0, bytes_out_ = 0, bytes_in_ = 0;
  std::deque<uint16_t> recent_seqs_;
  bool sent_hello_ = false, got_hello_ = false;
  Tap tap_ = nullptr;
  void* tap_ctx_ = nullptr;
};

}  // namespace btt
