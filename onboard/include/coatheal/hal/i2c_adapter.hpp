#pragma once

namespace coatheal {

class I2cAdapter {
 public:
  bool healthy() const { return healthy_; }
  void set_healthy(bool value) { healthy_ = value; }

 private:
  bool healthy_ = true;
};

}  // namespace coatheal