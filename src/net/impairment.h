#pragma once
#include <atomic>
#include <queue>
#include <random>

#include "net/transport.h"

namespace btt {

struct ImpairmentParams {
  int latency_ms = 0;      // one-way, added on send
  int jitter_ms = 0;       // +/- uniform
  double loss_pct = 0.0;   // 0..100
  double dup_pct = 0.0;    // duplicate probability
};

// Wraps any transport and degrades outgoing datagrams (latency, jitter -> reordering, loss,
// duplication). Parameters can be changed live from another thread.
class ImpairedTransport : public ITransport {
 public:
  explicit ImpairedTransport(TransportPtr inner) : inner_(std::move(inner)) {}
  void set_params(const ImpairmentParams& p);
  ImpairmentParams params() const;

  void update(double now) override;
  bool send(const uint8_t* d, size_t n) override;
  bool recv(std::vector<uint8_t>& out) override { return inner_->recv(out); }
  LinkState state() const override { return inner_->state(); }
  std::string join_code() const override { return inner_->join_code(); }
  std::string error() const override { return inner_->error(); }
  std::string name() const override { return inner_->name() + "+impaired"; }
  uint64_t peer_epoch() const override { return inner_->peer_epoch(); }
  void close() override { inner_->close(); }

 private:
  struct Pending {
    double due;
    uint64_t order;
    std::vector<uint8_t> data;
    bool operator>(const Pending& o) const { return due > o.due || (due == o.due && order > o.order); }
  };
  TransportPtr inner_;
  std::atomic<int> lat_{0}, jit_{0};
  std::atomic<double> loss_{0}, dup_{0};
  std::priority_queue<Pending, std::vector<Pending>, std::greater<Pending>> q_;
  std::mt19937 rng_{12345};
  double now_ = 0;
  uint64_t order_ = 0;
};

}  // namespace btt
