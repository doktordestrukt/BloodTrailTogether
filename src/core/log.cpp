#include "core/log.h"

#include <windows.h>

#include <chrono>
#include <share.h>

#include <cstdio>
#include <mutex>

namespace btt {
namespace {
std::mutex g_mu;
FILE* g_file = nullptr;
bool g_console = false;
LogLevel g_min = LogLevel::Info;
LogSink g_sink = nullptr;
const char* kNames[] = {"DBG", "INF", "WRN", "ERR"};
}  // namespace

void log_init(const std::wstring& path, bool console, LogLevel min_level) {
  std::lock_guard<std::mutex> lk(g_mu);
  if (g_file) fclose(g_file);
  g_file = nullptr;
  // Shared read access so the log can be viewed (or copied for a bug report) while the game runs.
  if (!path.empty()) g_file = _wfsopen(path.c_str(), L"w", _SH_DENYWR);
  // A second copy of the game is running (testing two players on one PC): the first copy holds the
  // log open, so this copy logs to <name>_2.log, _3.log ...
  for (int n = 2; !g_file && !path.empty() && n <= 8; ++n) {
    std::wstring alt = path;
    size_t dot = alt.rfind(L'.');
    alt.insert(dot == std::wstring::npos ? alt.size() : dot, L"_" + std::to_wstring(n));
    g_file = _wfsopen(alt.c_str(), L"w", _SH_DENYWR);
  }
  g_console = console;
  g_min = min_level;
}

void log_shutdown() {
  std::lock_guard<std::mutex> lk(g_mu);
  if (g_file) fclose(g_file);
  g_file = nullptr;
}

void log_set_sink(LogSink sink) {
  std::lock_guard<std::mutex> lk(g_mu);
  g_sink = sink;
}

void logf(LogLevel lvl, const char* fmt, ...) {
  if (lvl < g_min) return;
  char msg[2048];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(msg, sizeof(msg), fmt, ap);
  va_end(ap);

  SYSTEMTIME st;
  GetLocalTime(&st);
  char line[2200];
  snprintf(line, sizeof(line), "%02d:%02d:%02d.%03d [%s] %s\n", st.wHour, st.wMinute, st.wSecond,
           st.wMilliseconds, kNames[(int)lvl], msg);

  std::lock_guard<std::mutex> lk(g_mu);
  if (g_file) {
    fputs(line, g_file);
    fflush(g_file);
  }
  if (g_console) fputs(line, stdout);
  if (g_sink && lvl >= LogLevel::Info) g_sink(lvl, msg);
}

}  // namespace btt
