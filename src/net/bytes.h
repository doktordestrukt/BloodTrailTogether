#pragma once
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace btt {

// Little-endian writer/reader for BTT's own session protocol.
// (Relay framing has its own explicit byte order in relay_messages.cpp.)
class Writer {
 public:
  std::vector<uint8_t> buf;
  void u8(uint8_t v) { buf.push_back(v); }
  void u16(uint16_t v) { raw(&v, 2); }
  void u32(uint32_t v) { raw(&v, 4); }
  void i32(int32_t v) { raw(&v, 4); }
  void f32(float v) { raw(&v, 4); }
  void raw(const void* p, size_t n) {
    auto b = static_cast<const uint8_t*>(p);
    buf.insert(buf.end(), b, b + n);
  }
  void str(const std::string& s) {  // u16 length prefix, max 1024
    uint16_t n = (uint16_t)(s.size() > 1024 ? 1024 : s.size());
    u16(n);
    raw(s.data(), n);
  }
  size_t size() const { return buf.size(); }
  void patch_u16(size_t at, uint16_t v) { memcpy(&buf[at], &v, 2); }
};

class Reader {
 public:
  Reader(const uint8_t* d, size_t n) : d_(d), n_(n) {}
  bool ok() const { return ok_; }
  size_t left() const { return ok_ ? n_ - p_ : 0; }
  uint8_t u8() { uint8_t v = 0; raw(&v, 1); return v; }
  uint16_t u16() { uint16_t v = 0; raw(&v, 2); return v; }
  uint32_t u32() { uint32_t v = 0; raw(&v, 4); return v; }
  int32_t i32() { int32_t v = 0; raw(&v, 4); return v; }
  float f32() { float v = 0; raw(&v, 4); return v; }
  void raw(void* out, size_t n) {
    if (!ok_ || p_ + n > n_) { ok_ = false; memset(out, 0, n); return; }
    memcpy(out, d_ + p_, n);
    p_ += n;
  }
  std::string str() {
    uint16_t n = u16();
    if (!ok_ || p_ + n > n_ || n > 1024) { ok_ = false; return {}; }
    std::string s(reinterpret_cast<const char*>(d_ + p_), n);
    p_ += n;
    return s;
  }
  const uint8_t* cur() const { return d_ + p_; }
  void skip(size_t n) { if (p_ + n > n_) ok_ = false; else p_ += n; }

 private:
  const uint8_t* d_;
  size_t n_;
  size_t p_ = 0;
  bool ok_ = true;
};

}  // namespace btt
