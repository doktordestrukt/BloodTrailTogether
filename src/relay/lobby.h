#pragma once
// Server list via Unity Lobby (same Unity project and anonymous sign-in as Relay).
// The host publishes a public lobby carrying its Relay join code; everyone else lists lobbies and
// joins one with a click - nobody types codes. Endpoints verified against the live service:
//   POST /v1/create, POST /v1/query, POST /v1/{id}/heartbeat (204), DELETE /v1/{id} (204).
// A lobby that is not heartbeated for ~30 s disappears from queries, so a crashed host's game
// vanishes from the list by itself.
#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace btt {

struct ServerEntry {
  std::string id;
  std::string host_name;
  std::string map;        // short map name
  std::string join_code;  // Relay join code
  std::string version;    // mod version of the host
  int proto = 0;          // network protocol of the host (0 = unknown: listed by 0.4.0 before this field)
  int players = 1, max_players = 2;
};

class LobbyClient {
 public:
  LobbyClient(std::string project_id, std::string environment);
  ~LobbyClient();

  // Host side: keep a public listing while `open` (code known, nobody joined yet).
  void set_listing(bool open, const std::string& host_name, const std::string& map, const std::string& code);
  // Client side: ask for a fresh list (asynchronous).
  void request_refresh();

  std::vector<ServerEntry> servers();
  bool refreshing() const { return refreshing_; }
  double last_refresh() const { return last_refresh_; }  // now_sec() of the last finished query, 0 = never
  std::string last_error();
  bool listed() const { return listed_; }

 private:
  void loop();
  bool ensure_token();
  void do_refresh();
  void do_listing();

  std::string project_, env_;
  std::thread thread_;
  std::atomic<bool> run_{true};
  std::mutex mu_;
  // desired listing state (set by UI)
  bool want_open_ = false;
  std::string want_name_, want_map_, want_code_;
  // current
  std::string lobby_id_, listed_code_, listed_map_;
  double last_heartbeat_ = 0;
  std::atomic<bool> listed_{false};
  std::atomic<bool> refresh_req_{false};
  std::atomic<bool> refreshing_{false};
  std::atomic<double> last_refresh_{0};
  std::vector<ServerEntry> servers_;
  std::string error_;
  std::string token_;
  double token_expires_ = 0;
};

}  // namespace btt
