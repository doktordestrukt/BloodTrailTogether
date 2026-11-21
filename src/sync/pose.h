#pragma once
#include <cmath>
#include <cstdint>

namespace btt {

// Unreal conventions: centimetres, X forward, Y right, Z up; quaternions (x,y,z,w).
struct Vec3 {
  float x = 0, y = 0, z = 0;
};
struct Quat {
  float x = 0, y = 0, z = 0, w = 1;
};
struct Xform {
  Vec3 p;
  Quat q;
};

// One sample of a VR player's tracked devices in WORLD space.
struct PoseFrame {
  Xform head, left, right;
  Xform body;  // player pawn root (floor-level body position + yaw), drives the remote body model
  uint8_t flags = 0;  // bit0 left valid, bit1 right valid
  float height_scale = 1.0f;
};

inline Quat qnormalize(Quat q) {
  float n = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
  if (n < 1e-8f) return {};
  return {q.x / n, q.y / n, q.z / n, q.w / n};
}

inline Quat qslerp(Quat a, Quat b, float t) {
  float d = a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
  if (d < 0) { b = {-b.x, -b.y, -b.z, -b.w}; d = -d; }
  if (d > 0.9995f) {
    return qnormalize({a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t,
                       a.w + (b.w - a.w) * t});
  }
  float th = std::acos(d), s = std::sin(th);
  float wa = std::sin((1 - t) * th) / s, wb = std::sin(t * th) / s;
  return {a.x * wa + b.x * wb, a.y * wa + b.y * wb, a.z * wa + b.z * wb, a.w * wa + b.w * wb};
}

inline Vec3 vlerp(Vec3 a, Vec3 b, float t) {
  return {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t};
}

}  // namespace btt
