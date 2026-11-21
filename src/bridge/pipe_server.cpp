// Local IPC between the UE4SS Lua script (game thread) and this DLL.
// Two byte-mode named pipes per game process:
//   \\.\pipe\BloodTrailTogether_<n>_up    Lua -> DLL  (Lua opens with io.open(...,"wb"))
//   \\.\pipe\BloodTrailTogether_<n>_down  DLL -> Lua  (Lua opens with io.open(...,"rb"))
// Each frame is "%06d" decimal length + payload. Lua writes one request per game frame and
// immediately reads the reply; replies are built from cached state without blocking, so a
// round trip costs tens of microseconds. If Lua goes away (hot reload) we wait for it again.
#include <windows.h>

#include <cstdio>
#include <string>

#include "bridge/app.h"
#include "core/log.h"

namespace btt {

namespace {

bool read_exact(HANDLE h, char* buf, DWORD n) {
  DWORD got = 0;
  while (got < n) {
    DWORD r = 0;
    if (!ReadFile(h, buf + got, n - got, &r, nullptr) || r == 0) return false;
    got += r;
  }
  return true;
}

bool write_all(HANDLE h, const char* buf, DWORD n) {
  DWORD done = 0;
  while (done < n) {
    DWORD w = 0;
    if (!WriteFile(h, buf + done, n - done, &w, nullptr) || w == 0) return false;
    done += w;
  }
  return true;
}

HANDLE make_pipe(const std::wstring& name, DWORD dir) {
  return CreateNamedPipeW(name.c_str(), dir | FILE_FLAG_FIRST_PIPE_INSTANCE, PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
                          1, 1 << 16, 1 << 16, 0, nullptr);
}

DWORD WINAPI pipe_thread(LPVOID) {
  HANDLE up = INVALID_HANDLE_VALUE, down = INVALID_HANDLE_VALUE;
  int idx = 0;
  for (int n = 1; n <= 8; ++n) {
    std::wstring base = L"\\\\.\\pipe\\BloodTrailTogether_" + std::to_wstring(n);
    up = make_pipe(base + L"_up", PIPE_ACCESS_INBOUND);
    if (up == INVALID_HANDLE_VALUE) continue;
    down = make_pipe(base + L"_down", PIPE_ACCESS_OUTBOUND);
    if (down == INVALID_HANDLE_VALUE) {
      CloseHandle(up);
      continue;
    }
    idx = n;
    break;
  }
  if (!idx) {
    BTT_ERROR("bridge: could not create named pipes (another 8 game instances running?)");
    return 1;
  }
  app().pipe_index = idx;
  BTT_INFO("bridge: waiting for the Lua mod on pipe set %d", idx);

  std::string req, reply;
  for (;;) {
    BOOL c1 = ConnectNamedPipe(up, nullptr) || GetLastError() == ERROR_PIPE_CONNECTED;
    BOOL c2 = c1 && (ConnectNamedPipe(down, nullptr) || GetLastError() == ERROR_PIPE_CONNECTED);
    if (!c2) {
      Sleep(100);
      continue;
    }
    app().lua_connected = true;
    BTT_INFO("bridge: Lua mod connected");
    for (;;) {
      char hdr[7] = {};
      if (!read_exact(up, hdr, 6)) break;
      int len = atoi(hdr);
      if (len < 0 || len > (1 << 20)) break;
      req.resize(len);
      if (len && !read_exact(up, req.data(), len)) break;
      reply = lua_handle_request(req);
      char out[8];
      snprintf(out, sizeof(out), "%06d", (int)reply.size());
      if (!write_all(down, out, 6) || !write_all(down, reply.data(), (DWORD)reply.size())) break;
    }
    app().lua_connected = false;
    BTT_WARN("bridge: Lua mod disconnected (reload?), waiting again");
    DisconnectNamedPipe(up);
    DisconnectNamedPipe(down);
  }
}

}  // namespace

void pipe_server_start() {
  HANDLE t = CreateThread(nullptr, 0, pipe_thread, nullptr, 0, nullptr);
  if (t) CloseHandle(t);
}

}  // namespace btt
