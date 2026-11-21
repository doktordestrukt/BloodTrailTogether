#pragma once
#include <winsock2.h>
#include <ws2tcpip.h>

#include "net/transport.h"

namespace btt {

// Winsock lifetime helper (refcounted).
bool net_startup();
void net_cleanup();

// DEVELOPER transport: direct UDP between two processes (e.g. game + SimPeer on the same PC
// via 127.0.0.1). Not exposed in release builds; real players always use Unity Relay.
class UdpTransport : public ITransport {
 public:
  // host: bind to port and accept the first sender. join: send to host:port.
  static TransportPtr host(uint16_t port);
  static TransportPtr join(const std::string& host, uint16_t port);
  ~UdpTransport() override;

  void update(double now) override;
  bool send(const uint8_t* d, size_t n) override;
  bool recv(std::vector<uint8_t>& out) override;
  LinkState state() const override { return state_; }
  std::string join_code() const override { return code_; }
  std::string error() const override { return err_; }
  std::string name() const override { return "udp-dev"; }
  void close() override;

 private:
  UdpTransport() = default;
  SOCKET s_ = INVALID_SOCKET;
  sockaddr_in peer_{};
  bool have_peer_ = false;
  bool is_host_ = false;
  LinkState state_ = LinkState::Idle;
  std::string code_, err_;
};

}  // namespace btt
