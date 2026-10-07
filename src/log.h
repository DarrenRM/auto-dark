#pragma once
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <share.h>
#include <mutex>

// Timestamped, thread-safe logging to stdout (when there is one) and an optional log file.
inline FILE*& logFile() {
  static FILE* f = nullptr;
  return f;
}

inline void logToFile(const char* path) {
  if (logFile()) fclose(logFile());
  logFile() = _fsopen(path, "w", _SH_DENYNO);  // shared, so the log can be read while running
}

inline void logf(const char* fmt, ...) {
  static std::mutex mu;
  static const auto start = std::chrono::steady_clock::now();
  double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
  char msg[1024];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(msg, sizeof msg, fmt, ap);
  va_end(ap);
  std::lock_guard lk(mu);
  fprintf(stdout, "[%8.3f] %s\n", t, msg);
  fflush(stdout);
  if (FILE* f = logFile()) {
    fprintf(f, "[%8.3f] %s\n", t, msg);
    fflush(f);
  }
}
