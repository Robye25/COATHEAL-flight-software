#pragma once

#include <atomic>
#include <mutex>
#include <string>

#include "coatheal/status_flags.hpp"

namespace coatheal {

class StorageManager {
 public:
  explicit StorageManager(std::string primary_path, std::string secondary_path);

  bool Initialize(std::string* error);
  void WriteLine(const std::string& line);

  // Force both primary and secondary mirrors to stable storage (fsync on
  // POSIX; best-effort flush elsewhere). Safe to call from any thread.
  void FlushAndSync();

  // When SAFE mode is enabled, every WriteLine fsyncs both mirrors before
  // returning. Trades throughput for durability on unclean shutdown.
  void SetSafeMode(bool enabled) { safe_mode_.store(enabled); }
  bool safe_mode() const { return safe_mode_.load(); }

  StatusFlags status() const;

 private:
  static bool EnsureParentDirectory(const std::string& path);
  static void AppendLine(const std::string& path, const std::string& line,
                         bool write_header, bool sync, bool* ok);

  std::string primary_path_;
  std::string secondary_path_;
  mutable std::mutex mu_;
  bool primary_ok_ = true;
  bool secondary_ok_ = true;
  bool wrote_header_ = false;
  std::atomic<bool> safe_mode_{false};
};

}  // namespace coatheal
