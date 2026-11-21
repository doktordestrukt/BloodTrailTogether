#include "relay/relay_transport.h"

#include <cstring>

#include "core/log.h"
#include "core/util.h"
#include "net/udp_transport.h"

namespace btt {

using namespace relay;

namespace {
constexpr double kBindRetry = 1.0;
constexpr int kMaxBindAttempts = 10;
constexpr double kPingInterval = 1.0;
constexpr double kConnectRetry = 1.0;
constexpr int kMaxConnectAttempts = 15;
constexpr double kRelaySilenceFail = 12.0;  // relay echoes our pings; silence means we lost it
}  // namespace

RelayTransport::RelayTransport(const RelayConfig& cfg, bool is_host, std::string code, bool spawn_worker)
    : cfg_(cfg), is_host_(is_host), code_(std::move(code)) {
  net_startup();
  if (spawn_worker) worker_ = std::thread([this] { worker(); });
}

TransportPtr RelayTransport::host(const RelayConfig& cfg) {
  return TransportPtr(new RelayTransport(cfg, true, "", true));
}

TransportPtr RelayTransport::from_allocation(const AllocationInfo& info, bool is_host, const std::string& code) {
  auto t = new RelayTransport(RelayConfig{}, is_host, code, /*spawn_worker=*/false);
  t->alloc_ = info;
  t->worker_ok_ = true;
  t->worker_done_ = true;
  return TransportPtr(t);
}

TransportPtr RelayTransport::join(const RelayConfig& cfg, const std::string& join_code) {
  std::string code;
  for (char c : join_code)
    if (c != ' ' && c != '-' && c != '\t') code += (char)toupper((unsigned char)c);
  return TransportPtr(new RelayTransport(cfg, false, code, true));
}

RelayTransport::~RelayTransport() {
  close();
  if (worker_.joinable()) worker_.join();
  net_cleanup();
}

std::string RelayTransport::join_code() const {
  std::lock_guard<std::mutex> lk(mu_);
  return is_host_ ? code_ : std::string();
}

std::string RelayTransport::error() const {
  std::lock_guard<std::mutex> lk(mu_);
  return err_;
}

void RelayTransport::fail(const std::string& why) {
  {
    std::lock_guard<std::mutex> lk(mu_);
    err_ = why;
  }
  BTT_ERROR("relay: %s", why.c_str());
  state_ = LinkState::Failed;
}

// Control-plane work happens off the network thread (HTTPS can take hundreds of ms).
void RelayTransport::worker() {
  RelayApi api(cfg_.project_id, cfg_.environment);
  AuthToken tok;
  ApiError err;
  AllocationInfo info;
  std::string code;
  bool ok = api.sign_in(tok, err);
  if (ok) {
    if (is_host_) {
      ok = api.allocate(tok, 1, cfg_.region, info, err) && api.create_join_code(tok, info.allocation_id_str, code, err);
    } else {
      std::string c;
      {
        std::lock_guard<std::mutex> lk(mu_);
        c = code_;
      }
      if (c.empty()) {
        ok = false;
        err.message = "Please enter a join code";
      } else {
        ok = api.join(tok, c, info, err);
      }
    }
  }
  {
    std::lock_guard<std::mutex> lk(mu_);
    if (ok) {
      alloc_ = info;
      if (is_host_) code_ = code;
    } else {
      err_ = err.message;
    }
    worker_ok_ = ok;
  }
  worker_done_ = true;
}

bool RelayTransport::open_socket() {
  addrinfo hints{}, *res = nullptr;
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_DGRAM;
  hints.ai_protocol = IPPROTO_UDP;
  std::string port = std::to_string(alloc_.udp.port);
  if (getaddrinfo(alloc_.udp.host.c_str(), port.c_str(), &hints, &res) != 0 || !res) {
    fail("could not resolve relay server " + alloc_.udp.host);
    return false;
  }
  memcpy(&server_, res->ai_addr, res->ai_addrlen);
  server_len_ = (int)res->ai_addrlen;
  s_ = socket(res->ai_family, SOCK_DGRAM, IPPROTO_UDP);
  freeaddrinfo(res);
  if (s_ == INVALID_SOCKET) {
    fail("could not create UDP socket");
    return false;
  }
  u_long nb = 1;
  ioctlsocket(s_, FIONBIO, &nb);
  BOOL off = FALSE;
  DWORD ret = 0;
  WSAIoctl(s_, _WSAIOW(IOC_VENDOR, 12), &off, sizeof(off), nullptr, 0, &ret, nullptr, nullptr);  // SIO_UDP_CONNRESET
  return true;
}

void RelayTransport::send_raw(const std::vector<uint8_t>& v) {
  if (s_ == INVALID_SOCKET || v.empty()) return;
  sendto(s_, (const char*)v.data(), (int)v.size(), 0, (const sockaddr*)&server_, server_len_);
}

void RelayTransport::send_bind(double now) {
  send_raw(make_bind(nonce_, alloc_.connection_data, alloc_.key));
  last_bind_ = now;
  ++bind_attempts_;
}

void RelayTransport::update(double now) {
  now_ = now;
  if (state_ == LinkState::Failed || state_ == LinkState::Closed) return;

  if (state_ == LinkState::Preparing && s_ == INVALID_SOCKET) {
    if (!worker_done_) return;
    if (!worker_ok_) {
      state_ = LinkState::Failed;
      return;
    }
    if (!open_socket()) return;
    send_bind(now);
    state_ = is_host_ ? LinkState::Preparing : LinkState::Connecting;
    last_rx_ = now;
  }

  // Receive everything pending.
  uint8_t buf[2048];
  for (;;) {
    sockaddr_storage from{};
    int fl = sizeof(from);
    int r = recvfrom(s_, (char*)buf, sizeof(buf), 0, (sockaddr*)&from, &fl);
    if (r <= 0) break;
    last_rx_ = now;
    handle(buf, (size_t)r, now);
    if (state_ == LinkState::Failed) return;
  }

  if (!bound_) {
    if (now - last_bind_ >= kBindRetry) {
      if (bind_attempts_ >= kMaxBindAttempts) {
        fail("Relay server did not answer (UDP may be blocked by a firewall)");
        return;
      }
      ++nonce_;  // each new BIND must use a larger nonce
      send_bind(now);
    }
    return;
  }

  if (now - last_ping_ >= kPingInterval) {
    send_raw(make_ping(alloc_.allocation_id, ++ping_seq_));
    last_ping_ = now;
    ping_sent_at_ = now;
  }
  if (now - last_rx_ > kRelaySilenceFail) {
    fail("lost connection to the Relay server");
    return;
  }
  if (!is_host_ && !have_peer_ && now - last_connect_req_ >= kConnectRetry) {
    if (connect_attempts_ >= kMaxConnectAttempts) {
      fail("the host did not accept the connection (is the session still open?)");
      return;
    }
    send_raw(make_connect_request(alloc_.allocation_id, alloc_.host_connection_data));
    last_connect_req_ = now;
    ++connect_attempts_;
  }
}

void RelayTransport::handle(const uint8_t* d, size_t n, double now) {
  Parsed m;
  if (!parse(d, n, m)) return;
  switch (m.type) {
    case MsgType::BindReceived:
      if (!bound_) {
        bound_ = true;
        BTT_INFO("relay: bound to relay server (%s)", is_host_ ? "host" : "client");
        if (is_host_) state_ = LinkState::WaitingForPeer;
        last_ping_ = -1;
      }
      break;
    case MsgType::Ping:
      if (ping_sent_at_ >= 0) relay_rtt_ms_ = (now - ping_sent_at_) * 1000.0;
      break;
    case MsgType::Accepted:
      if (!is_host_ && m.b == alloc_.allocation_id) {
        peer_ = m.a;  // From = host
        have_peer_ = true;
        ++epoch_;
        state_ = LinkState::Ready;
        BTT_INFO("relay: host accepted the connection");
      }
      break;
    case MsgType::Rejected:
      fail("the host rejected the connection (session full?)");
      break;
    case MsgType::Relay:
      if (m.b != alloc_.allocation_id) break;
      if (is_host_) {
        if (!have_peer_ || m.a != peer_) {
          if (have_peer_) BTT_INFO("relay: a new player connected (replacing previous peer)");
          peer_ = m.a;
          have_peer_ = true;
          ++epoch_;
          state_ = LinkState::Ready;
        }
      } else if (!have_peer_ || m.a != peer_) {
        break;  // not from our host
      }
      if (inbox_.size() < 1024) inbox_.emplace_back(m.payload, m.payload + m.payload_len);
      break;
    case MsgType::Disconnect:
      if (have_peer_ && m.a == peer_) {
        if (is_host_) {
          BTT_INFO("relay: the other player disconnected; waiting for a new player");
          have_peer_ = false;
          state_ = LinkState::WaitingForPeer;
        } else {
          fail("the host closed the session");
        }
      }
      break;
    case MsgType::Error: {
      uint8_t code = m.error;
      BTT_WARN("relay: server error %u (%s)", code, error_text(code));
      if (code == (uint8_t)ErrorCode::ClientPlayerMismatch && bind_attempts_ < kMaxBindAttempts) {
        bound_ = false;  // our address changed or bind was lost: rebind with a new nonce
        ++nonce_;
        send_bind(now);
      } else if (code == (uint8_t)ErrorCode::NotConnected) {
        if (!is_host_) { have_peer_ = false; state_ = LinkState::Connecting; connect_attempts_ = 0; }
      } else if (code == (uint8_t)ErrorCode::Timeout || code == (uint8_t)ErrorCode::AllocationNotFound ||
                 code == (uint8_t)ErrorCode::Unauthorized || code == (uint8_t)ErrorCode::InvalidProtocolVersion) {
        fail(std::string("Relay: ") + error_text(code));
      }
      break;
    }
    default:
      break;
  }
}

bool RelayTransport::send(const uint8_t* d, size_t n) {
  if (state_ != LinkState::Ready || !have_peer_ || n > kMaxPayload) return false;
  send_raw(make_relay(alloc_.allocation_id, peer_, d, n));
  return true;
}

bool RelayTransport::recv(std::vector<uint8_t>& out) {
  if (inbox_.empty()) return false;
  out = std::move(inbox_.front());
  inbox_.pop_front();
  return true;
}

void RelayTransport::close() {
  if (s_ != INVALID_SOCKET) {
    for (int i = 0; i < 3; ++i) {
      if (have_peer_) send_raw(make_disconnect(alloc_.allocation_id, peer_));
      send_raw(make_close(alloc_.allocation_id));
    }
    closesocket(s_);
    s_ = INVALID_SOCKET;
  }
  if (state_ != LinkState::Failed) state_ = LinkState::Closed;
}

}  // namespace btt
