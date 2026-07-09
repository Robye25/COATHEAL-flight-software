#include "coatheal/storage_manager.hpp"

#include <cstdio>
#include <filesystem>

#if defined(__unix__) || defined(__APPLE__)
#include <unistd.h>
#endif

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

void StorageManager::AppendLine(const std::string& path, const std::string& line,
                                bool write_header, bool sync, bool* ok) {
  *ok = true;
  if (!EnsureParentDirectory(path)) {
    *ok = false;
    return;
  }
  std::FILE* fp = std::fopen(path.c_str(), "ab");
  if (fp == nullptr) {
    *ok = false;
    return;
  }
  if (write_header) {
    const char header[] = "# COATHEAL telemetry log\n";
    if (std::fwrite(header, 1, sizeof(header) - 1, fp) != sizeof(header) - 1) {
      *ok = false;
      std::fclose(fp);
      return;
    }
  }
  if (std::fwrite(line.data(), 1, line.size(), fp) != line.size() ||
      std::fputc('\n', fp) == EOF) {
    *ok = false;
    std::fclose(fp);
    return;
  }
  if (std::fflush(fp) != 0) {
    *ok = false;
    std::fclose(fp);
    return;
  }
  if (sync) {
#if defined(__unix__) || defined(__APPLE__)
    const int fd = ::fileno(fp);
    if (fd >= 0 && ::fsync(fd) != 0) {
      *ok = false;
    }
#endif
  }
  std::fclose(fp);
}

void StorageManager::WriteLine(const std::string& line) {
  std::lock_guard<std::mutex> lock(mu_);
  const bool sync = safe_mode_.load();
  const bool write_header = !wrote_header_;

  AppendLine(primary_path_, line, write_header, sync, &primary_ok_);
  AppendLine(secondary_path_, line, write_header, sync, &secondary_ok_);
  if (primary_ok_ || secondary_ok_) {
    wrote_header_ = true;
  }
}

void StorageManager::FlushAndSync() {
  std::lock_guard<std::mutex> lock(mu_);
#if defined(__unix__) || defined(__APPLE__)
  auto sync_path = [](const std::string& path, bool* ok) {
    std::FILE* fp = std::fopen(path.c_str(), "ab");
    if (fp == nullptr) {
      *ok = false;
      return;
    }
    std::fflush(fp);
    const int fd = ::fileno(fp);
    if (fd >= 0 && ::fsync(fd) != 0) {
      *ok = false;
    }
    std::fclose(fp);
  };
  sync_path(primary_path_, &primary_ok_);
  sync_path(secondary_path_, &secondary_ok_);
#endif
}

StatusFlags StorageManager::status() const {
  std::lock_guard<std::mutex> lock(mu_);
  StatusFlags flags;
  flags.sd_ok = primary_ok_;
  flags.usb_ok = secondary_ok_;
  return flags;
}

bool StorageManager::ActiveCheck(std::string* details) {
  std::lock_guard<std::mutex> lock(mu_);
  auto probe = [](const std::string& path) {
    std::FILE* fp = std::fopen(path.c_str(), "ab");
    if (fp == nullptr) return false;
    bool ok = std::fflush(fp) == 0;
#if defined(__unix__) || defined(__APPLE__)
    const int fd = ::fileno(fp);
    if (fd < 0 || ::fsync(fd) != 0) ok = false;
#endif
    std::fclose(fp);
    return ok;
  };
  primary_ok_ = probe(primary_path_);
  secondary_ok_ = probe(secondary_path_);
  if (details != nullptr) {
    *details = "sd=" + std::string(primary_ok_ ? "OK" : "FAIL") +
               ";usb=" + std::string(secondary_ok_ ? "OK" : "FAIL");
  }
  return primary_ok_ && secondary_ok_;
}

}  // namespace coatheal
