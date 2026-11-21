#pragma once
#include <string>

#include "relay/json.h"
#include "relay/relay_messages.h"

namespace btt::relay {

// Control plane (HTTPS). Endpoints taken from Unity's own SDKs:
//   Authentication: POST https://player-auth.services.api.unity.com/v1/authentication/anonymous
//                   headers ProjectId, UnityEnvironment  -> { idToken, sessionToken, userId, expiresIn }
//   Relay:          https://relay-allocations.services.api.unity.com
//                   POST /v1/allocate {maxConnections, region?}   -> data.allocation
//                   POST /v1/joincode {allocationId}              -> data.joinCode
//                   POST /v1/join     {joinCode}                  -> data.allocation (+hostConnectionData)
// Only the PUBLIC project id is needed. No service-account secret is ever shipped.

struct AuthToken {
  std::string id_token;
  std::string player_id;
  double expires_at = 0;  // now_sec() based
};

struct ServerEndpoint {
  std::string host;
  int port = 0;
  std::string connection_type;  // "udp", "dtls", "ws", "wss"
  bool secure = false;
};

struct AllocationInfo {
  std::string allocation_id_str;
  AllocationId allocation_id{};
  HmacKey key{};
  ConnectionData connection_data{};
  ConnectionData host_connection_data{};  // only for joins
  bool has_host_data = false;
  ServerEndpoint udp;  // chosen endpoint
  std::string region;
};

struct ApiError {
  std::string message;  // human-readable, suitable for UI
  int http_status = 0;
};

class RelayApi {
 public:
  RelayApi(std::string project_id, std::string environment) : project_(std::move(project_id)), env_(std::move(environment)) {}
  bool sign_in(AuthToken& out, ApiError& err);
  bool allocate(const AuthToken& tok, int max_connections, const std::string& region, AllocationInfo& out, ApiError& err);
  bool create_join_code(const AuthToken& tok, const std::string& allocation_id, std::string& code, ApiError& err);
  bool join(const AuthToken& tok, const std::string& join_code, AllocationInfo& out, ApiError& err);

  // Exposed for tests: parse an allocation JSON object.
  static bool parse_allocation(const btt::Json& alloc, bool is_join, AllocationInfo& out, std::string& why);

  // Overridable base URLs (tests / future proxy).
  std::string auth_base = "https://player-auth.services.api.unity.com";
  std::string relay_base = "https://relay-allocations.services.api.unity.com";

 private:
  std::string project_, env_;
};

}  // namespace btt::relay
