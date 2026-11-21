#include "net/loopback_transport.h"

namespace btt {

const char* link_state_name(LinkState s) {
  switch (s) {
    case LinkState::Idle: return "Idle";
    case LinkState::Preparing: return "Preparing";
    case LinkState::WaitingForPeer: return "WaitingForPeer";
    case LinkState::Connecting: return "Connecting";
    case LinkState::Ready: return "Ready";
    case LinkState::Failed: return "Failed";
    case LinkState::Closed: return "Closed";
  }
  return "?";
}

std::pair<TransportPtr, TransportPtr> LoopbackTransport::make_pair() {
  auto a = std::make_shared<Channel>();
  auto b = std::make_shared<Channel>();
  return {std::make_unique<LoopbackTransport>(a, b), std::make_unique<LoopbackTransport>(b, a)};
}

bool LoopbackTransport::send(const uint8_t* d, size_t n) {
  if (closed_ || n > kMaxPayload) return false;
  std::lock_guard<std::mutex> lk(out_->mu);
  if (out_->q.size() > 4096) return false;
  out_->q.emplace_back(d, d + n);
  return true;
}

bool LoopbackTransport::recv(std::vector<uint8_t>& out) {
  std::lock_guard<std::mutex> lk(in_->mu);
  if (in_->q.empty()) return false;
  out = std::move(in_->q.front());
  in_->q.pop_front();
  return true;
}

}  // namespace btt
