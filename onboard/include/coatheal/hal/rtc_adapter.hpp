#pragma once

#include <string>

namespace coatheal {

class RtcAdapter {
 public:
  bool valid() const { return valid_; }
  void set_valid(bool value) { valid_ = value; }

  std::string NowUtcIso8601() const;

 private:
  bool valid_ = true;
};

}  // namespace coatheal