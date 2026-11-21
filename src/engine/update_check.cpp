#include "engine/update_check.h"

#include "core/log.h"
#include "core/util.h"
#include "relay/http.h"
#include "relay/json.h"

namespace btt {

UpdateInfo check_for_update(const std::string& repo, const std::string& current_version) {
  UpdateInfo u;
  if (repo.empty() || repo.find('/') == std::string::npos) {
    u.error = "update checks disabled";
    return u;
  }
  auto r = http_request("GET", "https://api.github.com/repos/" + repo + "/releases/latest",
                        {{"Accept", "application/vnd.github+json"}, {"User-Agent", "BloodTrailTogether-UpdateCheck"}},
                        "", 8000);
  if (r.status != 200) {
    u.error = r.status == 0 ? r.error : "GitHub returned HTTP " + std::to_string(r.status);
    return u;
  }
  Json j;
  if (!Json::parse(r.body, j)) {
    u.error = "unexpected GitHub response";
    return u;
  }
  u.checked = true;
  u.latest_version = j["tag_name"].str();
  u.url = j["html_url"].str();
  u.available = !u.latest_version.empty() && semver_compare(u.latest_version, current_version) > 0;
  BTT_INFO("update check: latest %s, running %s%s", u.latest_version.c_str(), current_version.c_str(),
           u.available ? " (update available)" : "");
  return u;
}

}  // namespace btt
