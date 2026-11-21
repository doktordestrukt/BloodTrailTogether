#include "relay/http.h"

#include <windows.h>
#include <winhttp.h>

#include "core/util.h"

namespace btt {

HttpResponse http_request(const std::string& method, const std::string& url,
                          const std::vector<std::pair<std::string, std::string>>& headers,
                          const std::string& body, int timeout_ms) {
  HttpResponse res;
  std::wstring wurl = to_wide(url);
  URL_COMPONENTS uc{};
  uc.dwStructSize = sizeof(uc);
  wchar_t host[256], path[2048];
  uc.lpszHostName = host;
  uc.dwHostNameLength = 256;
  uc.lpszUrlPath = path;
  uc.dwUrlPathLength = 2048;
  if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &uc)) {
    res.error = "invalid URL";
    return res;
  }
  HINTERNET ses = WinHttpOpen(L"BloodTrailTogether/" BTT_VERSION_W, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                              WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
  if (!ses)  // Windows < 8.1 lacks AUTOMATIC_PROXY
    ses = WinHttpOpen(L"BloodTrailTogether", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
                      WINHTTP_NO_PROXY_BYPASS, 0);
  if (!ses) {
    res.error = "WinHttpOpen failed";
    return res;
  }
  WinHttpSetTimeouts(ses, timeout_ms, timeout_ms, timeout_ms, timeout_ms);
  HINTERNET con = WinHttpConnect(ses, host, uc.nPort, 0);
  HINTERNET req = con ? WinHttpOpenRequest(con, to_wide(method).c_str(), path, nullptr, WINHTTP_NO_REFERER,
                                           WINHTTP_DEFAULT_ACCEPT_TYPES,
                                           uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0)
                      : nullptr;
  if (!req) {
    res.error = "could not open HTTP request";
  } else {
    std::wstring hdrs;
    for (auto& [k, v] : headers) hdrs += to_wide(k) + L": " + to_wide(v) + L"\r\n";
    BOOL ok = WinHttpSendRequest(req, hdrs.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : hdrs.c_str(),
                                 hdrs.empty() ? 0 : (DWORD)-1L, (LPVOID)(body.empty() ? nullptr : body.data()),
                                 (DWORD)body.size(), (DWORD)body.size(), 0) &&
              WinHttpReceiveResponse(req, nullptr);
    if (!ok) {
      res.error = "network error " + std::to_string(GetLastError()) + " contacting " + to_utf8(host);
    } else {
      DWORD status = 0, sz = sizeof(status);
      WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                          &status, &sz, WINHTTP_NO_HEADER_INDEX);
      res.status = (int)status;
      for (;;) {
        DWORD avail = 0;
        if (!WinHttpQueryDataAvailable(req, &avail) || avail == 0) break;
        std::string chunk(avail, '\0');
        DWORD got = 0;
        if (!WinHttpReadData(req, chunk.data(), avail, &got) || got == 0) break;
        res.body.append(chunk.data(), got);
        if (res.body.size() > (4u << 20)) break;
      }
    }
  }
  if (req) WinHttpCloseHandle(req);
  if (con) WinHttpCloseHandle(con);
  WinHttpCloseHandle(ses);
  return res;
}

}  // namespace btt
