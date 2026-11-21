#include "net/session.h"

#include <windows.h>

#include <algorithm>
#include <random>

#include "core/log.h"
#include "core/util.h"
#include "net/bytes.h"
#include "sync/pose_codec.h"

namespace btt {

namespace {
constexpr uint8_t kMagic0 = 'B', kMagic1 = 'T', kWireVersion = 1;
constexpr size_t kHeaderSize = 16;
constexpr size_t kPacketBudget = 1100;

enum Chunk : uint8_t { CH_POSE = 1, CH_REL = 2, CH_PING = 3, CH_PONG = 4, CH_BYE = 5 };
enum RelMsg : uint8_t { M_HELLO = 1, M_WELCOME = 2, M_EVENT = 3 };

constexpr double kInterruptAfter = 2.0;
constexpr double kDisconnectAfter = 20.0;
constexpr double kHelloRetry = 0.5;
constexpr size_t kMaxRelQueue = 1024;

// uint16 sequence comparison with wraparound.
inline bool seq_less(uint16_t a, uint16_t b) { return (int16_t)(a - b) < 0; }

void write_event(Writer& w, const NetEvent& e) {
  w.u8((uint8_t)e.type);
  w.u32(e.t_ms);
  w.str(e.s1);
  w.str(e.s2);
  w.f32(e.v.x); w.f32(e.v.y); w.f32(e.v.z);
  w.f32(e.f0); w.f32(e.f1);
}

NetEvent read_event(Reader& r) {
  NetEvent e;
  e.type = (EventType)r.u8();
  e.t_ms = r.u32();
  e.s1 = r.str();
  e.s2 = r.str();
  e.v.x = r.f32(); e.v.y = r.f32(); e.v.z = r.f32();
  e.f0 = r.f32(); e.f1 = r.f32();
  return e;
}
}  // namespace

const char* session_state_name(SessionState s) {
  switch (s) {
    case SessionState::Idle: return "Idle";
    case SessionState::Linking: return "Linking";
    case SessionState::Handshaking: return "Handshaking";
    case SessionState::Connected: return "Connected";
    case SessionState::Interrupted: return "Interrupted";
    case SessionState::Disconnected: return "Disconnected";
  }
  return "?";
}

Session::Session(TransportPtr transport, Role role, std::string local_name, std::string mod_version)
    : transport_(std::move(transport)), role_(role), local_name_(std::move(local_name)),
      mod_version_(std::move(mod_version)) {
  std::random_device rd;
  sid_ = rd() | 1u;
}

Session::~Session() { close(); }

void Session::close() {
  if (state_ != SessionState::Disconnected && transport_ && transport_->state() == LinkState::Ready) {
    // Best-effort goodbye, sent a few times since it is unreliable.
    for (int i = 0; i < 3; ++i) {
      Writer w;
      w.u8(kMagic0); w.u8(kMagic1); w.u8(kWireVersion); w.u8(0);
      w.u32(sid_); w.u16(seq_++); w.u16(rel_expected_); w.u32(clock_ms());
      w.u8(CH_BYE); w.u16(0);
      transport_->send(w.buf.data(), w.buf.size());
    }
    transport_->update(now_sec());
  }
  if (transport_) transport_->close();
  state_ = SessionState::Disconnected;
}

std::string Session::error() const {
  if (!error_.empty()) return error_;
  return transport_ ? transport_->error() : std::string();
}

void Session::reset_peer() {
  rel_expected_ = 0;
  rel_buf_.clear();
  interp_.reset();
  recent_seqs_.clear();
  got_hello_ = false;
  // Restart our reliable stream too so the new peer receives a clean, gap-free sequence.
  rel_out_.clear();
  rel_next_ = 0;
  sent_hello_ = false;
}

void Session::set_local_pose(const PoseFrame& f) {
  local_pose_ = f;
  have_local_pose_ = true;
}

void Session::queue_rel(std::vector<uint8_t> body) {
  if (rel_out_.size() >= kMaxRelQueue) {
    BTT_WARN("reliable queue full (%zu), dropping oldest message", rel_out_.size());
    rel_out_.pop_front();
  }
  rel_out_.push_back({rel_next_++, std::move(body)});
}

void Session::send_event(NetEvent ev) {
  if (ev.t_ms == 0) ev.t_ms = clock_ms();
  Writer w;
  w.u8(M_EVENT);
  write_event(w, ev);
  queue_rel(std::move(w.buf));
}

void Session::pop_events(std::vector<NetEvent>& out) {
  out.insert(out.end(), std::make_move_iterator(inbox_.begin()), std::make_move_iterator(inbox_.end()));
  inbox_.clear();
}

bool Session::sample_remote(double now, PoseFrame& out) const {
  if (state_ != SessionState::Connected && state_ != SessionState::Interrupted) return false;
  return interp_.sample(now, out);
}

void Session::send_hello(bool welcome) {
  Writer w;
  w.u8(welcome ? M_WELCOME : M_HELLO);
  w.u16(kProtocolVersion);
  w.u8((uint8_t)role_);
  w.str(local_name_);
  w.str(mod_version_);
  queue_rel(std::move(w.buf));
  sent_hello_ = true;
}

void Session::update(double now) {
  clock_ = now;
  if (state_ == SessionState::Disconnected) return;
  transport_->update(now);

  LinkState ls = transport_->state();
  if (ls == LinkState::Failed || ls == LinkState::Closed) {
    if (error_.empty()) error_ = transport_->error().empty() ? "connection closed" : transport_->error();
    BTT_WARN("session: transport %s (%s)", link_state_name(ls), error_.c_str());
    state_ = SessionState::Disconnected;
    return;
  }

  std::vector<uint8_t> pkt;
  while (transport_->recv(pkt)) {
    handle_packet(pkt.data(), pkt.size(), now);
    if (state_ == SessionState::Disconnected) return;
  }

  ls = transport_->state();
  if (ls != LinkState::Ready) {
    if (state_ != SessionState::Linking && role_ == Role::Host) {
      // Host keeps its session (and join code) open for the next player.
      peer_sid_ = 0;
      reset_peer();
      peer_name_.clear();
    }
    state_ = SessionState::Linking;
    return;
  }
  if (state_ == SessionState::Linking) {
    state_ = SessionState::Handshaking;
    BTT_INFO("session: link ready (%s), handshaking as %s", transport_->name().c_str(),
             role_ == Role::Host ? "host" : "client");
  }
  // Client starts the handshake; host answers with WELCOME on HELLO.
  if (role_ == Role::Client && !sent_hello_) {
    send_hello(false);
    hello_sent_ = now;
  }

  if (last_rx_ >= 0) {
    double idle = now - last_rx_;
    if (state_ == SessionState::Connected && idle > kInterruptAfter) {
      state_ = SessionState::Interrupted;
      BTT_WARN("session: no packets for %.1fs, connection interrupted", idle);
    }
    if (idle > kDisconnectAfter && (state_ == SessionState::Interrupted || state_ == SessionState::Handshaking)) {
      error_ = "peer timed out";
      BTT_WARN("session: peer timed out");
      state_ = SessionState::Disconnected;
      return;
    }
  }
  flush(now, false);
}

void Session::flush(double now, bool force) {
  bool connected = state_ == SessionState::Connected || state_ == SessionState::Interrupted;
  bool pose_due = connected && have_local_pose_ && (last_pose_sent_ < 0 || now - last_pose_sent_ >= pose_interval_);
  bool ping_due = last_ping_ < 0 || now - last_ping_ >= 1.0;
  double rto = std::max(0.06, rtt_valid_ ? rtt_ * 1.5 + 0.02 : 0.25);
  bool rel_due = false;
  for (auto& r : rel_out_)
    if (r.last_sent < 0 || now - r.last_sent >= rto) { rel_due = true; break; }
  bool keepalive_due = last_tx_ < 0 || now - last_tx_ >= 0.1;
  if (!(force || pose_due || ping_due || rel_due || keepalive_due)) return;

  // Pack one or more packets (pose + pings + as many due reliable messages as fit).
  auto rel_it = rel_out_.begin();
  bool first = true;
  for (int guard = 0; guard < 16; ++guard) {
    Writer w;
    w.u8(kMagic0); w.u8(kMagic1); w.u8(kWireVersion); w.u8(0);
    w.u32(sid_);
    w.u16(seq_++);
    w.u16(rel_expected_);
    w.u32(clock_ms());
    if (first) {
      if (pose_due) {
        Writer pw;
        write_pose(pw, local_pose_);
        w.u8(CH_POSE); w.u16((uint16_t)pw.size()); w.raw(pw.buf.data(), pw.size());
        last_pose_sent_ = now;
      }
      if (ping_due) {
        w.u8(CH_PING); w.u16(4); w.u32(clock_ms());
        last_ping_ = now;
      }
    }
    bool more = false;
    for (; rel_it != rel_out_.end(); ++rel_it) {
      if (!(rel_it->last_sent < 0 || now - rel_it->last_sent >= rto)) continue;
      size_t need = 3 + 2 + rel_it->body.size();
      if (w.size() + need > kPacketBudget) { more = true; break; }
      w.u8(CH_REL); w.u16((uint16_t)(2 + rel_it->body.size()));
      w.u16(rel_it->id);
      w.raw(rel_it->body.data(), rel_it->body.size());
      rel_it->last_sent = now;
      if (++rel_it->sends == 8) BTT_WARN("reliable msg %u resent 8 times", rel_it->id);
    }
    if (transport_->send(w.buf.data(), w.size())) {
      ++pkts_out_;
      bytes_out_ += w.size();
      if (tap_) tap_(tap_ctx_, 0, w.buf.data(), w.size());
    }
    last_tx_ = now;
    first = false;
    if (!more) break;
  }
}

void Session::handle_packet(const uint8_t* d, size_t n, double now) {
  if (n < kHeaderSize || d[0] != kMagic0 || d[1] != kMagic1) return;
  if (d[2] != kWireVersion) {
    error_ = "incompatible BloodTrail Together version on the other side";
    return;
  }
  Reader r(d, n);
  r.skip(4);
  uint32_t sid = r.u32();
  uint16_t seq = r.u16();
  uint16_t ack = r.u16();
  uint32_t send_ms = r.u32();
  if (!r.ok()) return;
  if (tap_) tap_(tap_ctx_, 1, d, n);
  ++pkts_in_;
  bytes_in_ += n;

  if (peer_sid_ != sid) {
    if (peer_sid_ != 0) BTT_INFO("session: peer restarted (new session id), resynchronising");
    peer_sid_ = sid;
    reset_peer();
    if (state_ == SessionState::Connected || state_ == SessionState::Interrupted) state_ = SessionState::Handshaking;
  }
  last_rx_ = now;
  if (state_ == SessionState::Interrupted) {
    state_ = SessionState::Connected;
    BTT_INFO("session: connection restored");
  }
  recent_seqs_.push_back(seq);
  if (recent_seqs_.size() > 200) recent_seqs_.pop_front();

  // Cumulative ack: everything before `ack` was delivered.
  while (!rel_out_.empty() && seq_less(rel_out_.front().id, ack)) rel_out_.pop_front();

  while (r.left() >= 3) {
    uint8_t type = r.u8();
    uint16_t len = r.u16();
    if (len > r.left()) return;
    const uint8_t* body = r.cur();
    r.skip(len);
    switch (type) {
      case CH_POSE:
        if (state_ == SessionState::Connected || state_ == SessionState::Interrupted) {
          Reader pr(body, len);
          PoseFrame f = read_pose(pr);
          if (pr.ok()) interp_.push(send_ms, now, f);
        }
        break;
      case CH_PING: {
        Writer w;  // answer immediately
        w.u8(kMagic0); w.u8(kMagic1); w.u8(kWireVersion); w.u8(0);
        w.u32(sid_); w.u16(seq_++); w.u16(rel_expected_); w.u32(clock_ms());
        w.u8(CH_PONG); w.u16(4); w.raw(body, 4);
        if (transport_->send(w.buf.data(), w.size())) { ++pkts_out_; bytes_out_ += w.size(); }
        break;
      }
      case CH_PONG: {
        if (len < 4) break;
        uint32_t t;
        memcpy(&t, body, 4);
        double sample = (uint32_t)(clock_ms() - t) / 1000.0;
        if (sample < 10.0) {
          rtt_ = rtt_valid_ ? rtt_ * 0.8 + sample * 0.2 : sample;
          rtt_valid_ = true;
        }
        break;
      }
      case CH_REL:
        if (len >= 2) {
          uint16_t id;
          memcpy(&id, body, 2);
          handle_rel(id, body + 2, len - 2, now);
        }
        break;
      case CH_BYE:
        if (role_ == Role::Host) {
          if (!got_hello_) return;  // duplicate goodbye (sent 3x)
          BTT_INFO("session: '%s' left; waiting for a player to join", peer_name_.c_str());
          peer_sid_ = 0;
          reset_peer();
          peer_name_.clear();
          last_rx_ = -1;
          state_ = SessionState::Handshaking;
        } else {
          BTT_INFO("session: host left");
          error_ = "the host ended the session";
          state_ = SessionState::Disconnected;
        }
        return;
      default:
        break;
    }
  }
}

void Session::handle_rel(uint16_t id, const uint8_t* d, size_t n, double now) {
  if (seq_less(id, rel_expected_)) return;  // duplicate of something already delivered
  if (id != rel_expected_) {
    if (rel_buf_.size() < 2048) rel_buf_.emplace(id, std::vector<uint8_t>(d, d + n));
    return;
  }
  deliver_rel(std::vector<uint8_t>(d, d + n), now);
  ++rel_expected_;
  for (auto it = rel_buf_.find(rel_expected_); it != rel_buf_.end(); it = rel_buf_.find(rel_expected_)) {
    auto body = std::move(it->second);
    rel_buf_.erase(it);
    deliver_rel(body, now);
    ++rel_expected_;
  }
}

void Session::deliver_rel(const std::vector<uint8_t>& body, double now) {
  Reader r(body.data(), body.size());
  uint8_t type = r.u8();
  if (type == M_HELLO || type == M_WELCOME) {
    uint16_t proto = r.u16();
    uint8_t role = r.u8();
    std::string name = r.str();
    std::string ver = r.str();
    if (!r.ok()) return;
    if (proto != kProtocolVersion) {
      if (role_ == Role::Host) {
        // Tell the joiner which version we run (so their game can say "update"), but keep the
        // session open for a player with a matching version.
        if (type == M_HELLO) send_hello(true);
        BTT_WARN("session: %s tried to join with BloodTrail Together %s (protocol %u) - needs %s", name.c_str(),
                 ver.c_str(), proto, mod_version_.c_str());
        return;
      }
      bool they_newer = proto > kProtocolVersion;
      error_ = "Update needed: " + (name.empty() ? std::string("the host") : name) + " runs BloodTrail Together " + ver +
               ", you have " + mod_version_ + ". " +
               (they_newer ? "Install the newer version from the download page."
                           : "They need to update - send them the download page.");
      BTT_ERROR("%s", error_.c_str());
      state_ = SessionState::Disconnected;
      return;
    }
    if ((Role)role == role_) {
      error_ = "both players are hosts/clients";
      state_ = SessionState::Disconnected;
      return;
    }
    peer_name_ = name;
    peer_version_ = ver;
    got_hello_ = true;
    if (type == M_HELLO && role_ == Role::Host) send_hello(true);
    state_ = SessionState::Connected;
    BTT_INFO("session: connected to '%s' (BloodTrail Together %s)", name.c_str(), ver.c_str());
    flush(now, true);
    return;
  }
  if (type == M_EVENT) {
    NetEvent e = read_event(r);
    if (!r.ok()) return;
    // Age estimate: how long ago the sender created it (uses half RTT as one-way delay).
    e.age_s = rtt_valid_ ? rtt_ / 2 : 0;
    inbox_.push_back(std::move(e));
  }
}

SessionStats Session::stats(double now) const {
  SessionStats s;
  s.rtt_ms = rtt_ * 1000.0;
  s.jitter_ms = interp_.jitter_ms();
  s.pkts_in = pkts_in_;
  s.pkts_out = pkts_out_;
  s.bytes_in = bytes_in_;
  s.bytes_out = bytes_out_;
  s.reliable_pending = rel_out_.size();
  s.since_last_rx = last_rx_ < 0 ? -1 : now - last_rx_;
  if (recent_seqs_.size() > 10) {
    // Expected packets in window vs received (handles wrap via int16 deltas from first).
    uint16_t base = recent_seqs_.front();
    int mx = 0;
    for (auto q : recent_seqs_) mx = std::max(mx, (int)(int16_t)(q - base));
    double expected = mx + 1.0;
    s.loss_pct = std::clamp((1.0 - recent_seqs_.size() / expected) * 100.0, 0.0, 100.0);
  }
  return s;
}

}  // namespace btt
