#include "relay/relay_messages.h"

#include <cstring>

#include "relay/hmac.h"

namespace btt::relay {

const char* error_text(uint8_t code) {
  switch (code) {
    case 0: return "invalid protocol version";
    case 1: return "timed out due to inactivity";
    case 2: return "unauthorized";
    case 3: return "allocation/client mismatch (not bound)";
    case 4: return "allocation not found (expired)";
    case 5: return "not connected";
    case 6: return "self-connect not allowed";
  }
  return "unknown relay error";
}

static void header(std::vector<uint8_t>& v, MsgType t) {
  v.push_back(0xDA);
  v.push_back(0x72);
  v.push_back(0x00);
  v.push_back((uint8_t)t);
}

bool compute_bind_hmac(uint16_t nonce, const ConnectionData& conn, const HmacKey& key, uint8_t out[32]) {
  uint8_t msg[263] = {};
  msg[0] = 0xDA;
  msg[1] = 0x72;
  // msg[2] version 0, msg[3] type Bind (0), msg[4] accept mode 0
  msg[5] = (uint8_t)(nonce & 0xFF);
  msg[6] = (uint8_t)(nonce >> 8);
  msg[7] = 255;
  memcpy(msg + 8, conn.data(), 255);
  return hmac_sha256(key.data(), key.size(), msg, sizeof(msg), out);
}

std::vector<uint8_t> make_bind(uint16_t nonce, const ConnectionData& conn, const HmacKey& key) {
  std::vector<uint8_t> v;
  v.reserve(4 + 1 + 2 + 1 + 255 + 32);
  header(v, MsgType::Bind);
  v.push_back(0);  // AcceptMode::Auto
  v.push_back((uint8_t)(nonce & 0xFF));
  v.push_back((uint8_t)(nonce >> 8));
  v.push_back(255);
  v.insert(v.end(), conn.begin(), conn.end());
  uint8_t mac[32];
  compute_bind_hmac(nonce, conn, key, mac);
  v.insert(v.end(), mac, mac + 32);
  return v;
}

std::vector<uint8_t> make_ping(const AllocationId& self, uint16_t seq) {
  std::vector<uint8_t> v;
  header(v, MsgType::Ping);
  v.insert(v.end(), self.begin(), self.end());
  v.push_back((uint8_t)(seq & 0xFF));
  v.push_back((uint8_t)(seq >> 8));
  return v;
}

std::vector<uint8_t> make_connect_request(const AllocationId& self, const ConnectionData& host_conn) {
  std::vector<uint8_t> v;
  header(v, MsgType::ConnectRequest);
  v.insert(v.end(), self.begin(), self.end());
  v.push_back(255);
  v.insert(v.end(), host_conn.begin(), host_conn.end());
  return v;
}

std::vector<uint8_t> make_relay(const AllocationId& from, const AllocationId& to, const uint8_t* d, size_t n) {
  std::vector<uint8_t> v;
  if (n > kMaxRelayPayload) return v;
  v.reserve(38 + n);
  header(v, MsgType::Relay);
  v.insert(v.end(), from.begin(), from.end());
  v.insert(v.end(), to.begin(), to.end());
  v.push_back((uint8_t)(n >> 8));  // big-endian length
  v.push_back((uint8_t)(n & 0xFF));
  v.insert(v.end(), d, d + n);
  return v;
}

std::vector<uint8_t> make_disconnect(const AllocationId& from, const AllocationId& to) {
  std::vector<uint8_t> v;
  header(v, MsgType::Disconnect);
  v.insert(v.end(), from.begin(), from.end());
  v.insert(v.end(), to.begin(), to.end());
  return v;
}

std::vector<uint8_t> make_close(const AllocationId& self) {
  std::vector<uint8_t> v;
  header(v, MsgType::Close);
  v.insert(v.end(), self.begin(), self.end());
  return v;
}

bool parse(const uint8_t* d, size_t n, Parsed& out) {
  if (n < kHeader || d[0] != 0xDA || d[1] != 0x72 || d[2] != 0x00) return false;
  out = Parsed{};
  out.type = (MsgType)d[3];
  const uint8_t* p = d + 4;
  size_t left = n - 4;
  auto take_id = [&](AllocationId& id) {
    if (left < 16) return false;
    memcpy(id.data(), p, 16);
    p += 16;
    left -= 16;
    return true;
  };
  switch (out.type) {
    case MsgType::BindReceived:
      return true;
    case MsgType::Ping:
      return take_id(out.a);
    case MsgType::Accepted:
    case MsgType::Rejected:
    case MsgType::Disconnect:
      return take_id(out.a) && take_id(out.b);
    case MsgType::Relay: {
      if (!take_id(out.a) || !take_id(out.b) || left < 2) return false;
      size_t len = ((size_t)p[0] << 8) | p[1];
      p += 2;
      left -= 2;
      if (len > left) return false;
      out.payload = p;
      out.payload_len = len;
      return true;
    }
    case MsgType::Error:
      if (!take_id(out.a) || left < 1) return false;
      out.error = *p;
      return true;
    case MsgType::Close:
      return take_id(out.a);
    default:
      return false;
  }
}

}  // namespace btt::relay
