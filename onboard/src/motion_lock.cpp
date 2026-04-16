#include "coatheal/motion_lock.hpp"

#include <mutex>

namespace coatheal {

bool MotionLock::TryAcquire(int motor_id) {
  std::lock_guard<std::mutex> lk(mu_);
  if (holder_ != -1) {
    // Someone (possibly this same motor_id, which is a caller bug) already
    // holds it. Refuse. One motor at a time is a hard flight rule.
    return false;
  }
  holder_ = motor_id;
  return true;
}

void MotionLock::Release(int motor_id) {
  std::lock_guard<std::mutex> lk(mu_);
  // Only the holder can clear the lock. An unknown caller calling Release()
  // must NOT be able to open the heater path while another motor is still
  // moving — this would defeat the interlock.
  if (holder_ == motor_id) {
    holder_ = -1;
  }
}

int MotionLock::holder() const {
  std::lock_guard<std::mutex> lk(mu_);
  return holder_;
}

bool MotionLock::is_active() const {
  std::lock_guard<std::mutex> lk(mu_);
  return holder_ != -1;
}

}  // namespace coatheal
