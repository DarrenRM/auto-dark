#pragma once
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <mutex>

// Timestamped, thread-safe console logging.
inline void logf(const char* fmt, ...) {
  static std::mutex mu;
  static const auto start = std::chrono::steady_clock::now();
  double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
  std::lock_guard lk(mu);
  fprintf(stdout, "[%8.3f] ", t);
  va_list ap;
  va_start(ap, fmt);
  vfprintf(stdout, fmt, ap);
  va_end(ap);
  fputc('\n', stdout);
  fflush(stdout);
}
