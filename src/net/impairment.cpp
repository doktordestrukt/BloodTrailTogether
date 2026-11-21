#include "net/impairment.h"

namespace btt {

void ImpairedTransport::set_params(const ImpairmentParams& p) {
  lat_ = p.latency_ms;
  jit_ = p.jitter_ms;
  loss_ = p.loss_pct;
  dup_ = p.dup_pct;
}

ImpairmentParams ImpairedTransport::params() const {
  return {lat_.load(), jit_.load(), loss_.load(), dup_.load()};
}

void ImpairedTransport::update(double now) {
  now_ = now;
  inner_->update(now);
  while (!q_.empty() && q_.top().due <= now) {
    auto& p = q_.top();
    inner_->send(p.data.data(), p.data.size());
    q_.pop();
  }
}

bool ImpairedTransport::send(const uint8_t* d, size_t n) {
  std::uniform_real_distribution<double> u(0.0, 100.0);
  if (loss_ > 0 && u(rng_) < loss_) return true;  // silently dropped, like the internet
  int copies = (dup_ > 0 && u(rng_) < dup_) ? 2 : 1;
  for (int c = 0; c < copies; ++c) {
    int lat = lat_, jit = jit_;
    if (lat == 0 && jit == 0) {
      inner_->send(d, n);
      continue;
    }
    std::uniform_int_distribution<int> j(-jit, jit);
    double delay = (lat + (jit ? j(rng_) : 0)) / 1000.0;
    if (delay < 0) delay = 0;
    q_.push({now_ + delay, order_++, std::vector<uint8_t>(d, d + n)});
  }
  return true;
}

}  // namespace btt
