#pragma once

#include <string>
#include <vector>

#include "event.h"


class DMX {
 public:
  DMX(Event& event, const std::string& dev = "");
  ~DMX();
  void set(int idx, int val, bool fade = true);

 private:
  static const int FADE_TMO = 2500;

  void refresh(unsigned when = 1);
  void sendPacket();

  Event& event_;
  const std::string dev_;
  int fd_;
  std::vector<unsigned char> values_, phys_, updates_, fadeFrom_;
  int adj_, fadeTime_;
  void *refreshTmo_;
};
