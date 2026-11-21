#include "net/udp_transport.h"

#include <atomic>

#include "core/log.h"

namespace btt {

static std::atomic<int> g_wsa_refs{0};

bool net_startup() {
  if (g_wsa_refs++ == 0) {
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
      BTT_ERROR("WSAStartup failed");
      return false;
    }
  }
  return true;
}

void net_cleanup() {
  if (--g_wsa_refs == 0) WSACleanup();
}

static SOCKET make_socket(uint16_t bind_port, std::string& err) {
  SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (s == INVALID_SOCKET) {
    err = "socket() failed";
    return s;
  }
  u_long nb = 1;
  ioctlsocket(s, FIONBIO, &nb);
  // Ignore ICMP port-unreachable resets on UDP (WSAECONNRESET).
  BOOL off = FALSE;
  DWORD ret = 0;
  WSAIoctl(s, _WSAIOW(IOC_VENDOR, 12), &off, sizeof(off), nullptr, 0, &ret, nullptr, nullptr);
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_port = htons(bind_port);
  a.sin_addr.s_addr = htonl(bind_port ? INADDR_ANY : INADDR_ANY);
  if (bind(s, (sockaddr*)&a, sizeof(a)) != 0) {
    err = "bind() failed on port " + std::to_string(bind_port);
    closesocket(s);
    return INVALID_SOCKET;
  }
  return s;
}

TransportPtr UdpTransport::host(uint16_t port) {
  auto t = std::unique_ptr<UdpTransport>(new UdpTransport());
  net_startup();
  t->is_host_ = true;
  t->s_ = make_socket(port, t->err_);
  t->state_ = t->s_ == INVALID_SOCKET ? LinkState::Failed : LinkState::WaitingForPeer;
  t->code_ = "127.0.0.1:" + std::to_string(port);
  return t;
}

TransportPtr UdpTransport::join(const std::string& host, uint16_t port) {
  auto t = std::unique_ptr<UdpTransport>(new UdpTransport());
  net_startup();
  t->s_ = make_socket(0, t->err_);
  if (t->s_ == INVALID_SOCKET) {
    t->state_ = LinkState::Failed;
    return t;
  }
  t->peer_.sin_family = AF_INET;
  t->peer_.sin_port = htons(port);
  if (inet_pton(AF_INET, host.c_str(), &t->peer_.sin_addr) != 1) {
    t->err_ = "invalid address " + host;
    t->state_ = LinkState::Failed;
    return t;
  }
  t->have_peer_ = true;
  t->state_ = LinkState::Ready;
  return t;
}

UdpTransport::~UdpTransport() {
  close();
  net_cleanup();
}

void UdpTransport::update(double) {}

bool UdpTransport::send(const uint8_t* d, size_t n) {
  if (!have_peer_ || s_ == INVALID_SOCKET || n > kMaxPayload) return false;
  return sendto(s_, (const char*)d, (int)n, 0, (sockaddr*)&peer_, sizeof(peer_)) == (int)n;
}

bool UdpTransport::recv(std::vector<uint8_t>& out) {
  if (s_ == INVALID_SOCKET) return false;
  uint8_t buf[2048];
  sockaddr_in from{};
  int fl = sizeof(from);
  for (;;) {
    int r = recvfrom(s_, (char*)buf, sizeof(buf), 0, (sockaddr*)&from, &fl);
    if (r <= 0) return false;
    if (is_host_ && !have_peer_) {
      peer_ = from;
      have_peer_ = true;
      state_ = LinkState::Ready;
      BTT_INFO("udp-dev: peer connected");
    }
    if (from.sin_addr.s_addr != peer_.sin_addr.s_addr || from.sin_port != peer_.sin_port) {
      if (!is_host_) continue;  // ignore strangers
      peer_ = from;             // host: latest sender wins (peer restarted)
    }
    out.assign(buf, buf + r);
    return true;
  }
}

void UdpTransport::close() {
  if (s_ != INVALID_SOCKET) closesocket(s_);
  s_ = INVALID_SOCKET;
  state_ = LinkState::Closed;
}

}  // namespace btt
