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
  void toggle(int pin, bool slow = false);
  void i2c(int id, int bus, int dev, int addr, int bit);

 private:
  int getHandle(int pin, int mode);

  Event& event_;
  int fd_;
  std::map<int, std::array<int, 2> > handles_;
  std::map<int, std::array<int, 4> > i2c_;
  std::map<int, std::pair<int, unsigned long> > i2c_bus_handles_;
};
