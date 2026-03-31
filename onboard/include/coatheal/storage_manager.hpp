#pragma once

#include <mutex>
#include <string>

#include "coatheal/status_flags.hpp"

namespace coatheal {

class StorageManager {
 public:
  explicit StorageManager(std::string primary_path, std::string secondary_path);

  bool Initialize(std::string* error);
  void WriteLine(const std::string& line);

  StatusFlags status() const;

 private:
  static bool EnsureParentDirectory(const std::string& path);

  std::string primary_path_;
  std::string secondary_path_;
  mutable std::mutex mu_;
  bool primary_ok_ = true;
  bool secondary_ok_ = true;
  bool wrote_header_ = false;
};

}  // namespace coatheal