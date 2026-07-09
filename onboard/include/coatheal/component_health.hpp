#pragma once

#include <cstdint>
#include <string>

namespace coatheal {

enum class ComponentState {
  kDisabled,
  kDiscovering,
  kOk,
  kDegraded,
  kStale,
  kFailed,
};

inline const char* ToString(ComponentState state) {
  switch (state) {
    case ComponentState::kDisabled: return "DISABLED";
    case ComponentState::kDiscovering: return "DISCOVERING";
    case ComponentState::kOk: return "OK";
    case ComponentState::kDegraded: return "DEGRADED";
    case ComponentState::kStale: return "STALE";
    case ComponentState::kFailed: return "FAILED";
  }
  return "FAILED";
}

struct ComponentHealth {
  ComponentState state = ComponentState::kDiscovering;
  std::string error = "NOT_POLLED";
  std::int64_t last_success_age_ms = -1;
};

}  // namespace coatheal
