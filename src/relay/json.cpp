#include "relay/json.h"

#include <cstdlib>

namespace btt {

namespace {
const Json kNull;

struct P {
  const std::string& t;
  size_t i = 0;
  std::string err;
  void ws() {
    while (i < t.size() && (t[i] == ' ' || t[i] == '\t' || t[i] == '\n' || t[i] == '\r')) ++i;
  }
  bool fail(const char* m) {
    if (err.empty()) err = std::string(m) + " at " + std::to_string(i);
    return false;
  }
  static void utf8(std::string& o, unsigned cp) {
    if (cp < 0x80) o += (char)cp;
    else if (cp < 0x800) { o += (char)(0xC0 | (cp >> 6)); o += (char)(0x80 | (cp & 0x3F)); }
    else if (cp < 0x10000) { o += (char)(0xE0 | (cp >> 12)); o += (char)(0x80 | ((cp >> 6) & 0x3F)); o += (char)(0x80 | (cp & 0x3F)); }
    else { o += (char)(0xF0 | (cp >> 18)); o += (char)(0x80 | ((cp >> 12) & 0x3F)); o += (char)(0x80 | ((cp >> 6) & 0x3F)); o += (char)(0x80 | (cp & 0x3F)); }
  }
  bool string(std::string& o) {
    if (t[i] != '"') return fail("expected string");
    ++i;
    while (i < t.size()) {
      char c = t[i++];
      if (c == '"') return true;
      if (c != '\\') { o += c; continue; }
      if (i >= t.size()) break;
      char e = t[i++];
      switch (e) {
        case '"': o += '"'; break;
        case '\\': o += '\\'; break;
        case '/': o += '/'; break;
        case 'b': o += '\b'; break;
        case 'f': o += '\f'; break;
        case 'n': o += '\n'; break;
        case 'r': o += '\r'; break;
        case 't': o += '\t'; break;
        case 'u': {
          if (i + 4 > t.size()) return fail("bad \\u");
          unsigned cp = strtoul(t.substr(i, 4).c_str(), nullptr, 16);
          i += 4;
          if (cp >= 0xD800 && cp <= 0xDBFF && i + 6 <= t.size() && t[i] == '\\' && t[i + 1] == 'u') {
            unsigned lo = strtoul(t.substr(i + 2, 4).c_str(), nullptr, 16);
            i += 6;
            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
          }
          utf8(o, cp);
          break;
        }
        default: return fail("bad escape");
      }
    }
    return fail("unterminated string");
  }
  bool value(Json& v, int depth) {
    if (depth > 64) return fail("too deep");
    ws();
    if (i >= t.size()) return fail("unexpected end");
    char c = t[i];
    if (c == '{') {
      v.type = Json::Object;
      ++i;
      ws();
      if (i < t.size() && t[i] == '}') { ++i; return true; }
      for (;;) {
        ws();
        std::string k;
        if (!string(k)) return false;
        ws();
        if (i >= t.size() || t[i] != ':') return fail("expected ':'");
        ++i;
        if (!value(v.o[k], depth + 1)) return false;
        ws();
        if (i < t.size() && t[i] == ',') { ++i; continue; }
        if (i < t.size() && t[i] == '}') { ++i; return true; }
        return fail("expected ',' or '}'");
      }
    }
    if (c == '[') {
      v.type = Json::Array;
      ++i;
      ws();
      if (i < t.size() && t[i] == ']') { ++i; return true; }
      for (;;) {
        v.a.emplace_back();
        if (!value(v.a.back(), depth + 1)) return false;
        ws();
        if (i < t.size() && t[i] == ',') { ++i; continue; }
        if (i < t.size() && t[i] == ']') { ++i; return true; }
        return fail("expected ',' or ']'");
      }
    }
    if (c == '"') { v.type = Json::String; return string(v.s); }
    if (t.compare(i, 4, "true") == 0) { v.type = Json::Bool; v.b = true; i += 4; return true; }
    if (t.compare(i, 5, "false") == 0) { v.type = Json::Bool; v.b = false; i += 5; return true; }
    if (t.compare(i, 4, "null") == 0) { v.type = Json::Null; i += 4; return true; }
    char* end = nullptr;
    v.n = strtod(t.c_str() + i, &end);
    if (end == t.c_str() + i) return fail("unexpected character");
    v.type = Json::Number;
    i = end - t.c_str();
    return true;
  }
};
}  // namespace

bool Json::parse(const std::string& text, Json& out, std::string* err) {
  P p{text};
  out = Json();
  bool ok = p.value(out, 0);
  if (!ok && err) *err = p.err;
  return ok;
}

const Json& Json::operator[](const std::string& key) const {
  if (type != Object) return kNull;
  auto it = o.find(key);
  return it == o.end() ? kNull : it->second;
}

const Json& Json::operator[](size_t idx) const {
  if (type != Array || idx >= a.size()) return kNull;
  return a[idx];
}

std::string json_escape(const std::string& s) {
  std::string o;
  for (char c : s) {
    switch (c) {
      case '"': o += "\\\""; break;
      case '\\': o += "\\\\"; break;
      case '\n': o += "\\n"; break;
      case '\r': o += "\\r"; break;
      case '\t': o += "\\t"; break;
      default:
        if ((unsigned char)c < 0x20) { char b[8]; snprintf(b, sizeof(b), "\\u%04x", c); o += b; }
        else o += c;
    }
  }
  return o;
}

}  // namespace btt
