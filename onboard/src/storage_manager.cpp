#include "coatheal/storage_manager.hpp"

#include <filesystem>
#include <fstream>

namespace coatheal {

StorageManager::StorageManager(std::string primary_path, std::string secondary_path)
    : primary_path_(std::move(primary_path)), secondary_path_(std::move(secondary_path)) {}

bool StorageManager::EnsureParentDirectory(const std::string& path) {
  std::filesystem::path p(path);
  const auto parent = p.parent_path();
  if (parent.empty()) {
    return true;
  }
  std::error_code ec;
  std::filesystem::create_directories(parent, ec);
  return !ec;
}

bool StorageManager::Initialize(std::string* error) {
  const bool p_ok = EnsureParentDirectory(primary_path_);
  const bool s_ok = EnsureParentDirectory(secondary_path_);
  if (!p_ok || !s_ok) {
    if (error != nullptr) {
      *error = "failed to create storage directories";
    }
    return false;
  }
  return true;
}

void StorageManager::WriteLine(const std::string& line) {
  std::lock_guard<std::mutex> lock(mu_);

  bool any_wrote = false;

  auto append = [&](const std::string& path, bool* ok) {
    std::ofstream out(path, std::ios::app);
    if (!out.is_open()) {
      *ok = false;
      return;
    }
    if (!wrote_header_) {
      out << "# COATHEAL telemetry log\n";
    }
    out << line << '\n';
    if (!out.good()) {
      *ok = false;
      return;
    }
    any_wrote = true;
  };

  append(primary_path_, &primary_ok_);
  append(secondary_path_, &secondary_ok_);
  if (any_wrote) {
    wrote_header_ = true;
  }
}

StatusFlags StorageManager::status() const {
  std::lock_guard<std::mutex> lock(mu_);
  StatusFlags flags;
  flags.sd_ok = primary_ok_;
  flags.usb_ok = secondary_ok_;
  return flags;
}

}  // namespace coatheal