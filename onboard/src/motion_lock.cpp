#include "coatheal/motion_lock.hpp"

namespace coatheal {

// Trivial single-mutex implementation. Agent D will replace the body with the
// real interlock (including the heater-duty gate); the API is contractually
// stable.
bool MotionLock::TryAcquire(int motor_id) {
  std::lock_guard<std::mutex> lock(mu_);
  if (holder_ == -1 || holder_ == motor_id) {
    holder_ = motor_id;
    return true;
  }
  return false;
}

void MotionLock::Release(int motor_id) {
  std::lock_guard<std::mutex> lock(mu_);
  if (holder_ == motor_id) {
    holder_ = -1;
  }
}

int MotionLock::holder() const {
  std::lock_guard<std::mutex> lock(mu_);
  return holder_;
}

}  // namespace coatheal
