#pragma once
#include <array>
#include <cstdint>
#include <vector>

namespace btt::relay {

// Unity Relay message protocol.
// Sources: https://docs.unity.com/en-us/relay/relay-message-protocol (layout, type codes,
// error codes, 10 s inactivity timeout, 1400 B payload limit) and Unity Transport's reference
// implementation (com.unity.transport Runtime/Relay/*). Where the two differ, we follow the
// reference implementation because that is what Unity's own clients put on the wire:
//   * header bytes are DA 72 00 <type>
//   * BIND nonce and PING sequence are written little-endian
//   * RELAY payload length is big-endian
//   * BIND always carries 255 bytes of (zero-padded) connection data
//   * BIND HMAC = HMAC-SHA256(key[64], DA 72 00 00 00 nonce_lo nonce_hi FF conn[255])
enum class MsgType : uint8_t {
  Bind = 0,
  BindReceived = 1,
  Ping = 2,
  ConnectRequest = 3,
  Accepted = 6,
  Rejected = 8,
  Disconnect = 9,
  Relay = 10,
  Close = 11,
  Error = 12,
};

enum class ErrorCode : uint8_t {
  InvalidProtocolVersion = 0,
  Timeout = 1,
  Unauthorized = 2,
  ClientPlayerMismatch = 3,
  AllocationNotFound = 4,
  NotConnected = 5,
  SelfConnectNotAllowed = 6,
};
const char* error_text(uint8_t code);

using AllocationId = std::array<uint8_t, 16>;
using ConnectionData = std::array<uint8_t, 255>;
using HmacKey = std::array<uint8_t, 64>;

constexpr size_t kHeader = 4;
constexpr size_t kMaxRelayPayload = 1400;

std::vector<uint8_t> make_bind(uint16_t nonce, const ConnectionData& conn, const HmacKey& key);
std::vector<uint8_t> make_ping(const AllocationId& self, uint16_t seq);
std::vector<uint8_t> make_connect_request(const AllocationId& self, const ConnectionData& host_conn);
std::vector<uint8_t> make_relay(const AllocationId& from, const AllocationId& to, const uint8_t* d, size_t n);
std::vector<uint8_t> make_disconnect(const AllocationId& from, const AllocationId& to);
std::vector<uint8_t> make_close(const AllocationId& self);
bool compute_bind_hmac(uint16_t nonce, const ConnectionData& conn, const HmacKey& key, uint8_t out[32]);

struct Parsed {
  MsgType type{};
  AllocationId a{}, b{};  // meaning depends on type (from/to)
  uint8_t error = 0;
  const uint8_t* payload = nullptr;
  size_t payload_len = 0;
};
bool parse(const uint8_t* d, size_t n, Parsed& out);

}  // namespace btt::relay
