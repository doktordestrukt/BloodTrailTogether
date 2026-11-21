#pragma once
#include <cstddef>
#include <cstdint>

namespace btt {

// HMAC-SHA256 via Windows CNG (bcrypt). Returns false on failure.
bool hmac_sha256(const uint8_t* key, size_t key_len, const uint8_t* msg, size_t msg_len, uint8_t out[32]);

}  // namespace btt
