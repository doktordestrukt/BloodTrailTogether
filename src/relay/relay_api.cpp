#include "relay/relay_api.h"

#include <cstring>

#include "core/log.h"
#include "core/util.h"
#include "relay/http.h"
#include "relay/json.h"

namespace btt::relay {

namespace {

std::string describe_error(const HttpResponse& r, const std::string& what) {
  if (r.status == 0) return what + ": " + r.error + " (check your internet connection)";
  std::string detail;
  Json j;
  if (Json::parse(r.body, j)) {
    detail = j["detail"].str(j["title"].str(j["message"].str()));
    if (detail.empty()) detail = j["errors"][0]["message"].str();
  }
  std::string hint;
  if (r.status == 401 || r.status == 403)
    hint = " - the Unity project may not have Relay/Anonymous sign-in enabled";
  else if (r.status == 404)
    hint = " - join code not found or expired";
  else if (r.status == 429)
    hint = " - Unity rate limit reached, try again shortly";
  else if (r.status >= 500)
    hint = " - Unity service problem, try again later";
  return what + " failed (HTTP " + std::to_string(r.status) + ")" + (detail.empty() ? "" : ": " + detail) + hint;
}

template <size_t N>
bool b64_into(const Json& v, std::array<uint8_t, N>& out, bool allow_shorter, std::string& why, const char* field) {
  std::vector<uint8_t> bytes;
  if (v.type != Json::String || !base64_decode(v.s, bytes)) {
    why = std::string("missing/invalid ") + field;
    return false;
  }
  if (bytes.size() > N || (!allow_shorter && bytes.size() != N)) {
    why = std::string(field) + " has unexpected length " + std::to_string(bytes.size());
    return false;
  }
  out.fill(0);
  memcpy(out.data(), bytes.data(), bytes.size());
  return true;
}

}  // namespace

bool RelayApi::parse_allocation(const Json& a, bool is_join, AllocationInfo& out, std::string& why) {
  out = AllocationInfo{};
  out.allocation_id_str = a["allocationId"].str();
  out.region = a["region"].str();
  if (out.allocation_id_str.empty()) { why = "missing allocationId"; return false; }
  if (!b64_into(a["allocationIdBytes"], out.allocation_id, false, why, "allocationIdBytes")) {
    // Fall back to the textual UUID if the byte form is absent.
    if (!uuid_parse(out.allocation_id_str, out.allocation_id.data())) return false;
    why.clear();
  }
  if (!b64_into(a["key"], out.key, false, why, "key")) return false;
  if (!b64_into(a["connectionData"], out.connection_data, true, why, "connectionData")) return false;
  if (is_join) {
    if (!b64_into(a["hostConnectionData"], out.host_connection_data, true, why, "hostConnectionData")) return false;
    out.has_host_data = true;
  }
  // Prefer the plain "udp" endpoint (we implement the raw protocol, not DTLS).
  const Json& eps = a["serverEndpoints"];
  for (auto& e : eps.a) {
    if (e["connectionType"].str() == "udp") {
      out.udp.host = e["host"].str();
      out.udp.port = (int)e["port"].num();
      out.udp.connection_type = "udp";
      out.udp.secure = e["secure"].boolean();
    }
  }
  if (out.udp.host.empty()) {  // older responses: relayServer {ipV4, port}
    out.udp.host = a["relayServer"]["ipV4"].str();
    out.udp.port = (int)a["relayServer"]["port"].num();
    out.udp.connection_type = "udp";
  }
  if (out.udp.host.empty() || out.udp.port <= 0) { why = "no UDP relay endpoint in allocation"; return false; }
  return true;
}

bool RelayApi::sign_in(AuthToken& out, ApiError& err) {
  if (project_.empty()) {
    err.message = "No Unity project id configured (Relay.ProjectId in config.ini)";
    return false;
  }
  auto r = http_request("POST", auth_base + "/v1/authentication/anonymous",
                        {{"Content-Type", "application/json"}, {"ProjectId", project_}, {"UnityEnvironment", env_}},
                        "{}");
  if (r.status != 200) {
    err = {describe_error(r, "Unity sign-in"), r.status};
    return false;
  }
  Json j;
  if (!Json::parse(r.body, j) || j["idToken"].str().empty()) {
    err = {"Unity sign-in returned an unexpected response", r.status};
    return false;
  }
  out.id_token = j["idToken"].str();
  out.player_id = j["userId"].str();
  out.expires_at = now_sec() + j["expiresIn"].num(3600) - 60;
  BTT_INFO("relay: signed in anonymously (player %s)", out.player_id.c_str());
  return true;
}

bool RelayApi::allocate(const AuthToken& tok, int max_connections, const std::string& region, AllocationInfo& out,
                        ApiError& err) {
  std::string body = "{\"maxConnections\":" + std::to_string(max_connections);
  if (!region.empty()) body += ",\"region\":\"" + json_escape(region) + "\"";
  body += "}";
  auto r = http_request("POST", relay_base + "/v1/allocate",
                        {{"Content-Type", "application/json"}, {"Authorization", "Bearer " + tok.id_token}}, body);
  if (r.status != 201 && r.status != 200) {
    err = {describe_error(r, "Relay allocation"), r.status};
    return false;
  }
  Json j;
  std::string why;
  if (!Json::parse(r.body, j) || !parse_allocation(j["data"]["allocation"], false, out, why)) {
    err = {"Relay allocation response not understood: " + why, r.status};
    return false;
  }
  BTT_INFO("relay: allocation %s in region %s via %s:%d", out.allocation_id_str.c_str(), out.region.c_str(),
           out.udp.host.c_str(), out.udp.port);
  return true;
}

bool RelayApi::create_join_code(const AuthToken& tok, const std::string& allocation_id, std::string& code, ApiError& err) {
  auto r = http_request("POST", relay_base + "/v1/joincode",
                        {{"Content-Type", "application/json"}, {"Authorization", "Bearer " + tok.id_token}},
                        "{\"allocationId\":\"" + json_escape(allocation_id) + "\"}");
  if (r.status != 200 && r.status != 201) {
    err = {describe_error(r, "Join code creation"), r.status};
    return false;
  }
  Json j;
  if (!Json::parse(r.body, j) || j["data"]["joinCode"].str().empty()) {
    err = {"Join code response not understood", r.status};
    return false;
  }
  code = j["data"]["joinCode"].str();
  return true;
}

bool RelayApi::join(const AuthToken& tok, const std::string& join_code, AllocationInfo& out, ApiError& err) {
  auto r = http_request("POST", relay_base + "/v1/join",
                        {{"Content-Type", "application/json"}, {"Authorization", "Bearer " + tok.id_token}},
                        "{\"joinCode\":\"" + json_escape(join_code) + "\"}");
  if (r.status != 200 && r.status != 201) {
    err = {describe_error(r, "Joining with code " + join_code), r.status};
    return false;
  }
  Json j;
  std::string why;
  if (!Json::parse(r.body, j) || !parse_allocation(j["data"]["allocation"], true, out, why)) {
    err = {"Relay join response not understood: " + why, r.status};
    return false;
  }
  BTT_INFO("relay: joined allocation %s via %s:%d", out.allocation_id_str.c_str(), out.udp.host.c_str(), out.udp.port);
  return true;
}

}  // namespace btt::relay
