#pragma once
#include <winsock2.h>
#include <ws2tcpip.h>

#include <atomic>
#include <deque>
#include <mutex>
#include <thread>

#include "net/transport.h"
#include "relay/relay_api.h"

namespace btt {

struct RelayConfig {
  std::string project_id;
  std::string environment = "production";
  std::string region;
};

// Production transport: Unity Relay over UDP, implemented from the documented wire protocol.
//   host: sign in -> allocate(1 connection) -> join code -> BIND -> wait for joiner's RELAY data
//   join: sign in -> join(code) -> BIND -> CONNECT_REQUEST -> ACCEPTED -> RELAY data
// Keeps the allocation alive with PINGs every second (Relay drops clients after 10 s idle).
// Players only ever talk to Unity's relay server; neither side learns the other's IP address.
class RelayTransport : public ITransport {
 public:
  static TransportPtr host(const RelayConfig& cfg);
  static TransportPtr join(const RelayConfig& cfg, const std::string& join_code);
  // Tests only: skip the HTTPS control plane and use a ready-made allocation (mock relay server).
  static TransportPtr from_allocation(const relay::AllocationInfo& info, bool is_host, const std::string& code);
  ~RelayTransport() override;

  void update(double now) override;
  bool send(const uint8_t* d, size_t n) override;
  bool recv(std::vector<uint8_t>& out) override;
  LinkState state() const override { return state_; }
  std::string join_code() const override;
  std::string error() const override;
  std::string name() const override { return "unity-relay"; }
  uint64_t peer_epoch() const override { return epoch_; }
  void close() override;

  // Diagnostics
  double relay_rtt_ms() const { return relay_rtt_ms_; }
  std::string region() const { return alloc_.region; }

 private:
  RelayTransport(const RelayConfig& cfg, bool is_host, std::string code, bool spawn_worker);
  void worker();
  void fail(const std::string& why);
  bool open_socket();
  void send_raw(const std::vector<uint8_t>& v);
  void handle(const uint8_t* d, size_t n, double now);
  void send_bind(double now);

  RelayConfig cfg_;
  bool is_host_;
  std::thread worker_;
  std::atomic<bool> worker_done_{false};
  bool worker_ok_ = false;
  mutable std::mutex mu_;
  std::string code_, err_;

  relay::AllocationInfo alloc_;
  relay::AllocationId peer_{};
  bool have_peer_ = false;
  uint64_t epoch_ = 0;

  SOCKET s_ = INVALID_SOCKET;
  sockaddr_storage server_{};
  int server_len_ = 0;

  LinkState state_ = LinkState::Preparing;
  bool bound_ = false;
  uint16_t nonce_ = 0;
  int bind_attempts_ = 0;
  double last_bind_ = -1, last_ping_ = -1, last_connect_req_ = -1, last_rx_ = -1, now_ = 0;
  int connect_attempts_ = 0;
  uint16_t ping_seq_ = 0;
  double ping_sent_at_ = -1;
  double relay_rtt_ms_ = 0;
  std::deque<std::vector<uint8_t>> inbox_;
};

}  // namespace btt
