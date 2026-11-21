// btt_bridge_probe: emulates the UE4SS Lua side without the game.
// It loads btt_core.dll exactly like main.lua does (plain LoadLibrary, no exports used), then
// talks to it through the named pipes using C stdio fopen/fread/fwrite - the same calls Lua's
// io library makes - so the in-game IPC path is exercised for real.
//   btt_bridge_probe <path\to\Mods\BloodTrailTogether\bin\btt_core.dll> [seconds]
// Scenario: handshake, start the simulated player, stream a moving local pose, report a map and a
// sound, then check that a remote pose and the echoed sound come back.
#include <windows.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

static FILE* open_retry(const char* path, const char* mode) {
  for (int i = 0; i < 200; ++i) {
    FILE* f = nullptr;
    if (fopen_s(&f, path, mode) == 0 && f) return f;
    Sleep(25);
  }
  return nullptr;
}

int main(int argc, char** argv) {
  if (argc < 2) {
    printf("usage: btt_bridge_probe <btt_core.dll> [seconds]\n");
    return 2;
  }
  double seconds = argc > 2 ? atof(argv[2]) : 6.0;
  HMODULE dll = LoadLibraryA(argv[1]);
  if (!dll) {
    printf("FAIL: LoadLibrary error %lu\n", GetLastError());
    return 1;
  }
  FILE* up = open_retry("\\\\.\\pipe\\BloodTrailTogether_1_up", "wb");
  FILE* down = up ? open_retry("\\\\.\\pipe\\BloodTrailTogether_1_down", "rb") : nullptr;
  if (!up || !down) {
    printf("FAIL: could not open pipes\n");
    return 1;
  }
  setvbuf(up, nullptr, _IONBF, 0);
  setvbuf(down, nullptr, _IONBF, 0);

  auto roundtrip = [&](const std::string& req) -> std::string {
    char hdr[8];
    snprintf(hdr, sizeof(hdr), "%06d", (int)req.size());
    fwrite(hdr, 1, 6, up);
    fwrite(req.data(), 1, req.size(), up);
    char rh[7] = {};
    if (fread(rh, 1, 6, down) != 6) return "<eof>";
    int n = atoi(rh);
    std::string r(n, '\0');
    if (n && fread(r.data(), 1, n, down) != (size_t)n) return "<eof>";
    return r;
  };

  std::string r = roundtrip("H probe-1\n");
  printf("handshake reply:\n%s", r.c_str());
  bool cfg_ok = r.find("CFG ") != std::string::npos;
  roundtrip("K sim\nM /Game/Maps/Sandbox/Map_Sandbox\n");

  LARGE_INTEGER freq, t0, t1;
  QueryPerformanceFrequency(&freq);
  QueryPerformanceCounter(&t0);
  double worst_us = 0, total_us = 0;
  int frames = 0, remote_frames = 0, sounds_back = 0;
  int per_sec[60] = {};
  bool sound_sent = false;
  std::string last_rp, last_st;
  DWORD start = GetTickCount();
  while ((GetTickCount() - start) / 1000.0 < seconds) {
    double t = (GetTickCount() - start) / 1000.0;
    char pose[512];
    snprintf(pose, sizeof(pose),
             "P %.2f 0 170 0 0 0 1  %.2f -25 130 0 0 0 1  %.2f 25 %.2f 0 0 0 1  %.2f 0 95 0 0 0 1 3\nM /Game/Maps/Sandbox/Map_Sandbox\n",
             t * 50.0, t * 50.0 + 30, t * 50.0 + 30, 130 + 20 * std::sin(t * 4), t * 50.0);
    std::string req = pose;
    if (!sound_sent && t > 2.0) {
      req += "S /Game/WeaponMaster/Audio/Test/Cue_Test.Cue_Test 100 0 150 1.0 1.0\n";
      sound_sent = true;
    }
    QueryPerformanceCounter(&t1);
    r = roundtrip(req);
    LARGE_INTEGER t2;
    QueryPerformanceCounter(&t2);
    double us = (t2.QuadPart - t1.QuadPart) * 1e6 / freq.QuadPart;
    worst_us = us > worst_us ? us : worst_us;
    total_us += us;
    ++frames;
    if (r == "<eof>") {
      printf("FAIL: pipe closed\n");
      return 1;
    }
    size_t p = r.find("RP ");
    // "RP -" alone means no remote pose; "RP -12.3 ..." is a pose with a negative X.
    if (p != std::string::npos && r.compare(p, 5, "RP -\n") != 0) {
      ++remote_frames;
      last_rp = r.substr(p, r.find('\n', p) - p);
      int sec = (int)t;
      if (sec < 60) per_sec[sec]++;
    }
    size_t s = r.find("ST ");
    if (s != std::string::npos) last_st = r.substr(s, r.find('\n', s) - s);
    if (r.find("SN ") != std::string::npos) {
      ++sounds_back;
      printf("echoed sound: %s\n", r.substr(r.find("SN "), r.find('\n', r.find("SN ")) - r.find("SN ")).c_str());
    }
    size_t nt = r.find("NT ");
    if (nt != std::string::npos) printf("notification: %s\n", r.substr(nt + 3, r.find('\n', nt) - nt - 3).c_str());
    Sleep(11);  // ~90 Hz like a VR frame
  }
  printf("remote pose frames per second:");
  for (int i = 0; i < (int)seconds && i < 60; ++i) printf(" %d", per_sec[i]);
  printf("\n");
  printf("last status : %s\n", last_st.c_str());
  printf("last remote : %s\n", last_rp.c_str());
  printf("frames %d, frames with remote pose %d, round trip avg %.0f us, worst %.0f us\n", frames, remote_frames,
         total_us / frames, worst_us);
  bool ok = cfg_ok && remote_frames > frames / 2 && sounds_back == 1 && last_st.find("Connected") != std::string::npos;
  printf(ok ? "PROBE OK\n" : "PROBE FAILED\n");
  fflush(stdout);
  TerminateProcess(GetCurrentProcess(), ok ? 0 : 1);  // DLL threads are pinned; exit hard
  return 0;
}
