#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace btt {

enum class LinkState {
  Idle,
  Preparing,       // auth / allocation / socket setup
  WaitingForPeer,  // host: allocation bound, join code ready, nobody joined yet
  Connecting,      // joiner: connect request sent
  Ready,           // datagrams can flow to the peer
  Failed,
  Closed,
};

const char* link_state_name(LinkState s);

// A datagram link to exactly one remote peer. Unreliable, unordered, MTU <= kMaxPayload.
// Implementations: LoopbackTransport (tests/sim), UdpTransport (dev LAN/localhost),
// RelayTransport (Unity Relay, the production transport). Called from ONE thread only.
class ITransport {
 public:
  static constexpr size_t kMaxPayload = 1200;  // Relay allows 1400; keep headroom.
  virtual ~ITransport() = default;
  virtual void update(double now) = 0;
  virtual bool send(const uint8_t* data, size_t len) = 0;
  virtual bool recv(std::vector<uint8_t>& out) = 0;
  virtual LinkState state() const = 0;
  virtual std::string join_code() const { return {}; }
  virtual std::string error() const { return {}; }
  virtual std::string name() const = 0;
  // Peer may change identity (e.g. a new joiner on the same relay allocation).
  virtual uint64_t peer_epoch() const { return 0; }
  virtual void close() = 0;
};

using TransportPtr = std::unique_ptr<ITransport>;

}  // namespace btt
