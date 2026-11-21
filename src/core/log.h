#pragma once
#include <string>
#include <cstdarg>

namespace btt {

enum class LogLevel { Debug = 0, Info = 1, Warn = 2, Error = 3 };

// Thread-safe file + optional console logger.
void log_init(const std::wstring& path, bool console, LogLevel min_level);
void log_shutdown();
void logf(LogLevel lvl, const char* fmt, ...);
// Optional callback (e.g. UI) that receives every Info+ line.
using LogSink = void (*)(LogLevel, const char*);
void log_set_sink(LogSink sink);

}  // namespace btt

#define BTT_DEBUG(...) ::btt::logf(::btt::LogLevel::Debug, __VA_ARGS__)
#define BTT_INFO(...) ::btt::logf(::btt::LogLevel::Info, __VA_ARGS__)
#define BTT_WARN(...) ::btt::logf(::btt::LogLevel::Warn, __VA_ARGS__)
#define BTT_ERROR(...) ::btt::logf(::btt::LogLevel::Error, __VA_ARGS__)
