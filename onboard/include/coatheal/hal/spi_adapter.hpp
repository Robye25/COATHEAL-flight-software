#pragma once

namespace coatheal {

class SpiAdapter {
 public:
  bool healthy() const { return healthy_; }
  void set_healthy(bool value) { healthy_ = value; }

 private:
  bool healthy_ = true;
};

}  // namespace coatheal