#include "relay/lobby.h"

#include <windows.h>

#include "core/log.h"
#include "core/util.h"
#include "net/session.h"
#include "relay/http.h"
#include "relay/json.h"
#include "relay/relay_api.h"

namespace btt {

namespace {
const char* kLobbyBase = "https://lobby.services.api.unity.com/v1";
constexpr double kHeartbeatEvery = 10.0;  // the service drops lobbies after ~30 s without one
}  // namespace

LobbyClient::LobbyClient(std::string project_id, std::string environment)
    : project_(std::move(project_id)), env_(std::move(environment)) {
  thread_ = std::thread([this] { loop(); });
}

LobbyClient::~LobbyClient() {
  run_ = false;
  if (thread_.joinable()) thread_.join();
}

void LobbyClient::set_listing(bool open, const std::string& host_name, const std::string& map, const std::string& code) {
  std::lock_guard<std::mutex> lk(mu_);
  want_open_ = open && !code.empty();
  want_name_ = host_name;
  want_map_ = map;
  want_code_ = code;
}

void LobbyClient::request_refresh() { refresh_req_ = true; }

std::vector<ServerEntry> LobbyClient::servers() {
  std::lock_guard<std::mutex> lk(mu_);
  return servers_;
}

std::string LobbyClient::last_error() {
  std::lock_guard<std::mutex> lk(mu_);
  return error_;
}

bool LobbyClient::ensure_token() {
  if (!token_.empty() && now_sec() < token_expires_) return true;
  relay::RelayApi api(project_, env_);
  relay::AuthToken tok;
  relay::ApiError err;
  if (!api.sign_in(tok, err)) {
    std::lock_guard<std::mutex> lk(mu_);
    error_ = err.message;
    return false;
  }
  token_ = tok.id_token;
  token_expires_ = tok.expires_at;
  return true;
}

void LobbyClient::do_refresh() {
  refreshing_ = true;
  std::vector<ServerEntry> list;
  std::string err;
  if (ensure_token()) {
    auto r = http_request("POST", std::string(kLobbyBase) + "/query",
                          {{"Content-Type", "application/json"}, {"Authorization", "Bearer " + token_}},
                          "{\"count\":50}");
    Json j;
    if (r.status == 200 && Json::parse(r.body, j)) {
      for (auto& l : j["results"].a) {
        ServerEntry e;
        e.id = l["id"].str();
        e.host_name = l["name"].str();
        e.join_code = l["data"]["code"]["value"].str();
        e.map = l["data"]["map"]["value"].str();
        e.version = l["data"]["ver"]["value"].str();
        e.proto = atoi(l["data"]["proto"]["value"].str("0").c_str());
        e.max_players = (int)l["maxPlayers"].num(2);
        e.players = e.max_players - (int)l["availableSlots"].num(1);
        if (e.join_code.empty()) continue;  // not one of ours / incomplete
        if (e.id == lobby_id_) continue;    // my own listing
        list.push_back(e);
      }
    } else {
      err = r.status == 0 ? "Server list: " + r.error
                          : "Server list failed (HTTP " + std::to_string(r.status) + ")" +
                                (r.status == 403 || r.status == 401 ? " - enable Lobby in the Unity dashboard" : "");
      BTT_WARN("lobby: %s %s", err.c_str(), r.body.substr(0, 200).c_str());
    }
  } else {
    std::lock_guard<std::mutex> lk(mu_);
    err = error_;
  }
  {
    std::lock_guard<std::mutex> lk(mu_);
    if (err.empty()) servers_ = std::move(list);
    error_ = err;
  }
  last_refresh_ = now_sec();
  refreshing_ = false;
}

void LobbyClient::do_listing() {
  bool open;
  std::string name, map, code;
  {
    std::lock_guard<std::mutex> lk(mu_);
    open = want_open_;
    name = want_name_;
    map = want_map_;
    code = want_code_;
  }
  double now = now_sec();
  // close / replace a listing that no longer matches
  if (!lobby_id_.empty() && (!open || code != listed_code_)) {
    if (ensure_token())
      http_request("DELETE", std::string(kLobbyBase) + "/" + lobby_id_, {{"Authorization", "Bearer " + token_}}, "");
    BTT_INFO("lobby: listing %s removed", lobby_id_.c_str());
    lobby_id_.clear();
    listed_code_.clear();
    listed_ = false;
  }
  if (open && lobby_id_.empty()) {
    if (!ensure_token()) return;
    std::string title = (name.empty() ? std::string("Player") : name) + "'s game";
    std::string body = "{\"name\":\"" + json_escape(title) + "\",\"maxPlayers\":2,\"isPrivate\":false,\"data\":{" +
                       "\"code\":{\"value\":\"" + json_escape(code) + "\",\"visibility\":\"public\"}," +
                       "\"map\":{\"value\":\"" + json_escape(map) + "\",\"visibility\":\"public\"}," +
                       "\"ver\":{\"value\":\"" + BTT_VERSION + "\",\"visibility\":\"public\"}," +
                       "\"proto\":{\"value\":\"" + std::to_string(kProtocolVersion) + "\",\"visibility\":\"public\"}}}";
    auto r = http_request("POST", std::string(kLobbyBase) + "/create",
                          {{"Content-Type", "application/json"}, {"Authorization", "Bearer " + token_}}, body);
    Json j;
    if ((r.status == 200 || r.status == 201) && Json::parse(r.body, j) && !j["id"].str().empty()) {
      lobby_id_ = j["id"].str();
      listed_code_ = code;
      listed_map_ = map;
      last_heartbeat_ = now;
      listed_ = true;
      BTT_INFO("lobby: listed as \"%s\" (%s)", title.c_str(), lobby_id_.c_str());
    } else {
      std::lock_guard<std::mutex> lk(mu_);
      error_ = "Could not list your game (HTTP " + std::to_string(r.status) + ")" +
               (r.status == 403 || r.status == 401 ? " - enable Lobby in the Unity dashboard" : "");
      BTT_WARN("lobby: create failed: %d %s", r.status, r.body.substr(0, 200).c_str());
      Sleep(3000);  // don't hammer the service
    }
    return;
  }
  if (!lobby_id_.empty() && now - last_heartbeat_ > kHeartbeatEvery) {
    last_heartbeat_ = now;
    if (ensure_token()) {
      auto r = http_request("POST", std::string(kLobbyBase) + "/" + lobby_id_ + "/heartbeat",
                            {{"Content-Type", "application/json"}, {"Authorization", "Bearer " + token_}}, "{}");
      if (r.status == 404) {  // expired (e.g. the PC slept): list again
        lobby_id_.clear();
        listed_ = false;
      }
    }
  }
}

void LobbyClient::loop() {
  while (run_) {
    if (refresh_req_.exchange(false)) do_refresh();
    do_listing();
    for (int i = 0; i < 10 && run_ && !refresh_req_; ++i) Sleep(20);
  }
  // leave nothing behind when the game closes
  if (!lobby_id_.empty() && !token_.empty())
    http_request("DELETE", std::string(kLobbyBase) + "/" + lobby_id_, {{"Authorization", "Bearer " + token_}}, "", 3000);
}

}  // namespace btt
