#pragma once
#include <deque>
#include <memory>
#include <mutex>

#include "net/transport.h"

namespace btt {

// In-memory datagram pipe between two endpoints (thread-safe). Used for the in-game
// simulated player and for unit tests. Not used for real multiplayer.
class LoopbackTransport : public ITransport {
 public:
  struct Channel {
    std::mutex mu;
    std::deque<std::vector<uint8_t>> q;
  };
  static std::pair<TransportPtr, TransportPtr> make_pair();

  LoopbackTransport(std::shared_ptr<Channel> in, std::shared_ptr<Channel> out) : in_(in), out_(out) {}
  void update(double) override {}
  bool send(const uint8_t* d, size_t n) override;
  bool recv(std::vector<uint8_t>& out) override;
  LinkState state() const override { return closed_ ? LinkState::Closed : LinkState::Ready; }
  std::string name() const override { return "loopback"; }
  void close() override { closed_ = true; }

 private:
  std::shared_ptr<Channel> in_, out_;
  bool closed_ = false;
};

}  // namespace btt
