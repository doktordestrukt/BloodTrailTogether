#include "relay/hmac.h"

#include <windows.h>
#include <bcrypt.h>

namespace btt {

bool hmac_sha256(const uint8_t* key, size_t key_len, const uint8_t* msg, size_t msg_len, uint8_t out[32]) {
  BCRYPT_ALG_HANDLE alg = nullptr;
  BCRYPT_HASH_HANDLE h = nullptr;
  bool ok = false;
  if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, BCRYPT_ALG_HANDLE_HMAC_FLAG) != 0)
    return false;
  if (BCryptCreateHash(alg, &h, nullptr, 0, (PUCHAR)key, (ULONG)key_len, 0) == 0 &&
      BCryptHashData(h, (PUCHAR)msg, (ULONG)msg_len, 0) == 0 && BCryptFinishHash(h, out, 32, 0) == 0)
    ok = true;
  if (h) BCryptDestroyHash(h);
  BCryptCloseAlgorithmProvider(alg, 0);
  return ok;
}

}  // namespace btt
