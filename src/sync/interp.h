#pragma once
#include <cstdint>
#include <deque>

#include "sync/pose.h"

namespace btt {

// Snapshot interpolation buffer for the remote player's pose.
//  - Samples are stamped with the SENDER's clock (ms).
//  - The sender->local clock offset is estimated as the minimum observed (recv - send) over a
//    sliding window, which tracks the fastest path and rejects jitter spikes.
//  - Rendering happens `delay` behind the newest estimate so we usually interpolate between two
//    real samples; when packets stop we extrapolate (velocity) for at most `max_extrap`, then hold.
class PoseInterpolator {
 public:
  void configure(double delay_s, double max_extrap_s) { delay_ = delay_s; max_extrap_ = max_extrap_s; }
  void reset();
  void push(uint32_t sender_ms, double local_recv_s, const PoseFrame& f);
  // Returns false when no data yet / data too stale (> 3 s).
  bool sample(double local_now_s, PoseFrame& out) const;
  double last_recv() const { return last_recv_; }
  double jitter_ms() const { return jitter_ms_; }

 private:
  struct Snap {
    double t;  // sender time in seconds (unwrapped)
    PoseFrame f;
  };
  std::deque<Snap> snaps_;
  std::deque<double> offsets_;  // recv - send samples
  double offset_ = 0;
  double delay_ = 0.1, max_extrap_ = 0.15;
  double last_recv_ = -1;
  double jitter_ms_ = 0;
  uint32_t last_sender_ms_ = 0;
  double unwrap_base_ = 0;
  bool have_ = false;
};

}  // namespace btt
