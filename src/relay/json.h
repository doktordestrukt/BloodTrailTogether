#pragma once
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace btt {

// Small JSON DOM, sufficient for Unity Services and GitHub API responses.
struct Json {
  enum Type { Null, Bool, Number, String, Array, Object } type = Null;
  bool b = false;
  double n = 0;
  std::string s;
  std::vector<Json> a;
  std::map<std::string, Json> o;

  static bool parse(const std::string& text, Json& out, std::string* err = nullptr);
  const Json& operator[](const std::string& key) const;
  const Json& operator[](size_t i) const;
  bool is_null() const { return type == Null; }
  std::string str(const std::string& def = "") const { return type == String ? s : def; }
  double num(double def = 0) const { return type == Number ? n : def; }
  bool boolean(bool def = false) const { return type == Bool ? b : def; }
};

std::string json_escape(const std::string& s);

}  // namespace btt
