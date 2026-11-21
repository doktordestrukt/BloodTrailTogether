#pragma once
#include <string>
#include <utility>
#include <vector>

namespace btt {

struct HttpResponse {
  int status = 0;          // 0 = transport failure
  std::string body;
  std::string error;       // set when status == 0
};

// Synchronous HTTPS request via WinHTTP (system proxy aware, TLS from the OS).
// Call from a worker thread, never from the game thread.
HttpResponse http_request(const std::string& method, const std::string& url,
                          const std::vector<std::pair<std::string, std::string>>& headers,
                          const std::string& body, int timeout_ms = 10000);

}  // namespace btt
