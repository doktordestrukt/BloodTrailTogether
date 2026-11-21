#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace btt {

// Monotonic seconds since process start.
double now_sec();
uint32_t now_ms32();

std::string to_utf8(const std::wstring& w);
std::wstring to_wide(const std::string& s);

bool base64_decode(const std::string& in, std::vector<uint8_t>& out);
std::string base64_encode(const uint8_t* d, size_t n);
std::string hex(const uint8_t* d, size_t n);

// Parses a canonical UUID string into 16 bytes in textual (big-endian) order.
bool uuid_parse(const std::string& s, uint8_t out[16]);

// Directory containing the module (DLL/EXE) that contains this code.
std::wstring module_dir();

// Replaces whitespace with '_' so a value fits in one space-separated token.
std::string token_safe(std::string s);

// Simple semantic version compare ("v1.2.3" / "1.2.3"). Returns -1, 0, 1.
int semver_compare(const std::string& a, const std::string& b);

}  // namespace btt
