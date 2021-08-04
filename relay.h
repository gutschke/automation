#pragma once

#include <set>
#include <string>

#include "event.h"


class Relay {
 public:
  Relay(Event& event,
        const std::string& deviceName = "/dev/gpiochip0",
        const std::string& helper = "/usr/bin/gpio");
  ~Relay();

  void set(int pin, bool state);
  bool get(int pin);
  void toggle(int pin);

 private:
  Event& event_;
  int fd_;
  const std::string helper_;
  std::set<int> configured_;
};
