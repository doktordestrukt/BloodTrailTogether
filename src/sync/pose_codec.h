#pragma once
#include "net/bytes.h"
#include "sync/pose.h"

namespace btt {

// Compact pose encoding: positions as int32 millimetres (exact to 1 mm anywhere in a map),
// rotations as "smallest three" packed in 32 bits (~0.1 deg error). 6 transforms -> 97 bytes.
void write_quat32(Writer& w, Quat q);
Quat read_quat32(Reader& r);
void write_pose(Writer& w, const PoseFrame& f);
PoseFrame read_pose(Reader& r);

}  // namespace btt
