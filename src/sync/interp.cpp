#include "sync/interp.h"

#include <algorithm>
#include <cmath>

namespace btt {

void PoseInterpolator::reset() {
  snaps_.clear();
  offsets_.clear();
  have_ = false;
  last_recv_ = -1;
  jitter_ms_ = 0;
  unwrap_base_ = 0;
}

void PoseInterpolator::push(uint32_t sender_ms, double local_recv_s, const PoseFrame& f) {
  // Unwrap the 32-bit millisecond clock.
  if (have_ && sender_ms < last_sender_ms_ && (last_sender_ms_ - sender_ms) > 0x80000000u)
    unwrap_base_ += 4294967.296;
  // Older (reordered) packets are still inserted below in time order.
  double t = unwrap_base_ + sender_ms / 1000.0;
  if (!have_ || sender_ms > last_sender_ms_ || (last_sender_ms_ - sender_ms) > 0x80000000u)
    last_sender_ms_ = sender_ms;
  have_ = true;

  double off = local_recv_s - t;
  offsets_.push_back(off);
  if (offsets_.size() > 120) offsets_.pop_front();
  double mn = *std::min_element(offsets_.begin(), offsets_.end());
  // Jitter estimate: spread of offsets above the minimum.
  double sum = 0;
  for (double o : offsets_) sum += (o - mn);
  jitter_ms_ = sum / offsets_.size() * 1000.0;
  offset_ = mn;
  last_recv_ = local_recv_s;

  // Insert sorted by sender time; drop duplicates.
  auto it = std::lower_bound(snaps_.begin(), snaps_.end(), t, [](const Snap& s, double v) { return s.t < v; });
  if (it != snaps_.end() && std::fabs(it->t - t) < 1e-6) return;
  snaps_.insert(it, {t, f});
  while (snaps_.size() > 64) snaps_.pop_front();
}

static Xform lerp_x(const Xform& a, const Xform& b, float u) { return {vlerp(a.p, b.p, u), qslerp(a.q, b.q, u)}; }

static Xform extrap_x(const Xform& a, const Xform& b, double dt_ab, double dt) {
  // Linear velocity extrapolation of position; rotation held at the newest sample (rotational
  // extrapolation of hands overshoots badly in VR).
  Xform r = b;
  if (dt_ab > 1e-4) {
    float k = (float)(dt / dt_ab);
    r.p = {b.p.x + (b.p.x - a.p.x) * k, b.p.y + (b.p.y - a.p.y) * k, b.p.z + (b.p.z - a.p.z) * k};
  }
  return r;
}

bool PoseInterpolator::sample(double local_now_s, PoseFrame& out) const {
  if (snaps_.empty()) return false;
  if (local_now_s - last_recv_ > 3.0) return false;
  double render_t = local_now_s - offset_ - delay_;
  const Snap& newest = snaps_.back();
  if (render_t <= snaps_.front().t) {
    out = snaps_.front().f;
    return true;
  }
  if (render_t >= newest.t) {
    if (snaps_.size() < 2) {
      out = newest.f;
      return true;
    }
    const Snap& prev = snaps_[snaps_.size() - 2];
    double dt = std::min(render_t - newest.t, max_extrap_);
    out = newest.f;
    double dab = newest.t - prev.t;
    out.head = extrap_x(prev.f.head, newest.f.head, dab, dt);
    out.left = extrap_x(prev.f.left, newest.f.left, dab, dt);
    out.right = extrap_x(prev.f.right, newest.f.right, dab, dt);
    out.body = extrap_x(prev.f.body, newest.f.body, dab, dt);
    return true;
  }
  auto it = std::lower_bound(snaps_.begin(), snaps_.end(), render_t, [](const Snap& s, double v) { return s.t < v; });
  const Snap& b = *it;
  const Snap& a = *(it - 1);
  float u = (float)((render_t - a.t) / std::max(1e-6, b.t - a.t));
  out.flags = b.f.flags;
  out.height_scale = b.f.height_scale;
  out.head = lerp_x(a.f.head, b.f.head, u);
  out.left = lerp_x(a.f.left, b.f.left, u);
  out.right = lerp_x(a.f.right, b.f.right, u);
  out.body = lerp_x(a.f.body, b.f.body, u);
  return true;
}

}  // namespace btt
