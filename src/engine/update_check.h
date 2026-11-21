#pragma once
#include <string>

namespace btt {

struct UpdateInfo {
  bool checked = false;
  bool available = false;
  std::string latest_version;  // tag, e.g. "v0.2.0"
  std::string url;             // release page (opened only when the user clicks)
  std::string error;
};

// Queries https://api.github.com/repos/<repo>/releases/latest. Never downloads or installs
// anything; it only reports whether a newer release exists.
UpdateInfo check_for_update(const std::string& repo, const std::string& current_version);

}  // namespace btt
