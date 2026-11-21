#include "sync/pose_codec.h"

#include <algorithm>
#include <cmath>

namespace btt {

static constexpr float kQMax = 0.70710678f;  // max |component| of the non-largest three
static constexpr int kQBits = 10;
static constexpr int kQSteps = (1 << kQBits) - 1;

void write_quat32(Writer& w, Quat q) {
  q = qnormalize(q);
  float c[4] = {q.x, q.y, q.z, q.w};
  int big = 0;
  for (int i = 1; i < 4; ++i)
    if (std::fabs(c[i]) > std::fabs(c[big])) big = i;
  float sign = c[big] < 0 ? -1.0f : 1.0f;  // q and -q are the same rotation
  uint32_t packed = (uint32_t)big << 30;
  int shift = 20;
  for (int i = 0; i < 4; ++i) {
    if (i == big) continue;
    float v = std::clamp(c[i] * sign, -kQMax, kQMax);
    uint32_t qv = (uint32_t)std::lround((v + kQMax) / (2 * kQMax) * kQSteps);
    packed |= (qv & kQSteps) << shift;
    shift -= kQBits;
  }
  w.u32(packed);
}

Quat read_quat32(Reader& r) {
  uint32_t packed = r.u32();
  int big = (int)(packed >> 30);
  float c[4];
  float sum = 0;
  int shift = 20;
  for (int i = 0; i < 4; ++i) {
    if (i == big) continue;
    uint32_t qv = (packed >> shift) & kQSteps;
    shift -= kQBits;
    c[i] = (float)qv / kQSteps * (2 * kQMax) - kQMax;
    sum += c[i] * c[i];
  }
  c[big] = std::sqrt(std::max(0.0f, 1.0f - sum));
  return qnormalize({c[0], c[1], c[2], c[3]});
}

static void write_pos(Writer& w, Vec3 p) {
  w.i32((int32_t)std::lround(p.x * 10.0));  // cm -> mm
  w.i32((int32_t)std::lround(p.y * 10.0));
  w.i32((int32_t)std::lround(p.z * 10.0));
}

static Vec3 read_pos(Reader& r) {
  Vec3 v;
  v.x = r.i32() / 10.0f;
  v.y = r.i32() / 10.0f;
  v.z = r.i32() / 10.0f;
  return v;
}

void write_pose(Writer& w, const PoseFrame& f) {
  w.u8(f.flags);
  for (const Xform* x : {&f.head, &f.left, &f.right, &f.body, &f.heldL, &f.heldR}) {
    write_pos(w, x->p);
    write_quat32(w, x->q);
  }
}

PoseFrame read_pose(Reader& r) {
  PoseFrame f;
  f.flags = r.u8();
  for (Xform* x : {&f.head, &f.left, &f.right, &f.body, &f.heldL, &f.heldR}) {
    x->p = read_pos(r);
    x->q = read_quat32(r);
  }
  return f;
}

}  // namespace btt
