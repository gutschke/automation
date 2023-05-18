#pragma once

#include <array>
#include <map>
#include <string>

#include "event.h"


class Relay {
 public:
  Relay(Event& event,
        const std::string& deviceName = "/dev/gpiochip0");
  ~Relay();

  void set(int pin, bool state, int bias = -1);
  bool get(int pin, int bias = -1);
  void toggle(int pin);

 private:
  int getHandle(int pin, int mode);

  Event& event_;
  int fd_;
  std::map<int, std::array<int, 2> > handles_;
};
