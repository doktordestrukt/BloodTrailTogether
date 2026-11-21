#include "core/util.h"

#include <windows.h>

#include <chrono>
#include <cstdio>
#include <sstream>

namespace btt {

static const auto g_start = std::chrono::steady_clock::now();

double now_sec() {
  return std::chrono::duration<double>(std::chrono::steady_clock::now() - g_start).count();
}

uint32_t now_ms32() { return (uint32_t)(uint64_t)(now_sec() * 1000.0); }

std::string to_utf8(const std::wstring& w) {
  if (w.empty()) return {};
  int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
  std::string s(n, '\0');
  WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
  return s;
}

std::wstring to_wide(const std::string& s) {
  if (s.empty()) return {};
  int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
  std::wstring w(n, L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
  return w;
}

static const char* kB64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

bool base64_decode(const std::string& in, std::vector<uint8_t>& out) {
  out.clear();
  int val = 0, bits = -8;
  for (char c : in) {
    if (c == '=' || c == '\n' || c == '\r') continue;
    int d;
    if (c >= 'A' && c <= 'Z') d = c - 'A';
    else if (c >= 'a' && c <= 'z') d = c - 'a' + 26;
    else if (c >= '0' && c <= '9') d = c - '0' + 52;
    else if (c == '+' || c == '-') d = 62;
    else if (c == '/' || c == '_') d = 63;
    else return false;
    val = (val << 6) | d;
    bits += 6;
    if (bits >= 0) {
      out.push_back((uint8_t)((val >> bits) & 0xFF));
      bits -= 8;
    }
  }
  return true;
}

std::string base64_encode(const uint8_t* d, size_t n) {
  std::string out;
  int val = 0, bits = -6;
  for (size_t i = 0; i < n; ++i) {
    val = (val << 8) | d[i];
    bits += 8;
    while (bits >= 0) {
      out.push_back(kB64[(val >> bits) & 0x3F]);
      bits -= 6;
    }
  }
  if (bits > -6) out.push_back(kB64[((val << 8) >> (bits + 8)) & 0x3F]);
  while (out.size() % 4) out.push_back('=');
  return out;
}

std::string hex(const uint8_t* d, size_t n) {
  static const char* h = "0123456789abcdef";
  std::string s;
  s.reserve(n * 2);
  for (size_t i = 0; i < n; ++i) {
    s.push_back(h[d[i] >> 4]);
    s.push_back(h[d[i] & 15]);
  }
  return s;
}

bool uuid_parse(const std::string& s, uint8_t out[16]) {
  int n = 0;
  for (size_t i = 0; i < s.size() && n < 32; ++i) {
    char c = s[i];
    if (c == '-') continue;
    int v;
    if (c >= '0' && c <= '9') v = c - '0';
    else if (c >= 'a' && c <= 'f') v = c - 'a' + 10;
    else if (c >= 'A' && c <= 'F') v = c - 'A' + 10;
    else return false;
    if (n % 2 == 0) out[n / 2] = (uint8_t)(v << 4);
    else out[n / 2] |= (uint8_t)v;
    ++n;
  }
  return n == 32;
}

std::wstring module_dir() {
  HMODULE mod = nullptr;
  GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                     reinterpret_cast<LPCWSTR>(&module_dir), &mod);
  wchar_t buf[MAX_PATH * 2];
  DWORD n = GetModuleFileNameW(mod, buf, (DWORD)std::size(buf));
  std::wstring p(buf, n);
  auto pos = p.find_last_of(L"\\/");
  return pos == std::wstring::npos ? L"." : p.substr(0, pos);
}

std::string token_safe(std::string s) {
  for (auto& c : s)
    if (c == ' ' || c == '\t' || c == '\n' || c == '\r') c = '_';
  if (s.empty()) s = "-";
  return s;
}

int semver_compare(const std::string& a, const std::string& b) {
  auto parse = [](std::string v, int out[3]) {
    out[0] = out[1] = out[2] = 0;
    if (!v.empty() && (v[0] == 'v' || v[0] == 'V')) v = v.substr(1);
    sscanf_s(v.c_str(), "%d.%d.%d", &out[0], &out[1], &out[2]);
  };
  int x[3], y[3];
  parse(a, x);
  parse(b, y);
  for (int i = 0; i < 3; ++i) {
    if (x[i] < y[i]) return -1;
    if (x[i] > y[i]) return 1;
  }
  return 0;
}

}  // namespace btt
