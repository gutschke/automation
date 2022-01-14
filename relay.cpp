#include <fcntl.h>
#include <fmt/format.h>
#include <linux/gpio.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "relay.h"
#include "util.h"


Relay::Relay(Event& event, const std::string& deviceName,
             const std::string& helper)
  : event_(event),
    fd_(open(deviceName.c_str(), O_RDONLY)),
    helper_(helper) {
}

Relay::~Relay() {
  if (fd_ >= 0) {
    close(fd_);
  }
}

void Relay::set(int pin, bool state) {
  DBG("Relay::set(" << pin << ", " << (state ? "true" : "false") << ")");
  struct gpiohandle_request req = { };
  req.lineoffsets[0] = pin;
  req.lines = 1;
  req.flags = GPIOHANDLE_REQUEST_OUTPUT;
  if (fd_ >= 0 && !ioctl(fd_, GPIO_GET_LINEHANDLE_IOCTL, &req) &&
      req.fd >= 0) {
    struct gpiohandle_data data = { };
    data.values[0] = state;
    ioctl(req.fd, GPIOHANDLE_SET_LINE_VALUES_IOCTL, &data);
    close(req.fd);
  }
}

bool Relay::get(int pin) {
  if (configured_.find(pin) == configured_.end()) {
    // The GPIO driver in the Linux kernel knows nothing about advanced GPIO
    // features such as pull-up/down resistors. Traditionally, this requires
    // directly programming the chipset registers. But instead, we rely on
    // an external helper program.
    configured_.insert(pin);
    if (system(fmt::format("{} mode {} input >/dev/null 2>&1",
                           helper_, pin).c_str()));
    if (system(fmt::format("{} mode {} up >/dev/null 2>&1",
                           helper_, pin).c_str()));
  }
  struct gpiohandle_request req = { };
  req.lineoffsets[0] = pin;
  req.lines = 1;
  req.flags = GPIOHANDLE_REQUEST_INPUT;
  if (fd_ >= 0 && !ioctl(fd_, GPIO_GET_LINEHANDLE_IOCTL, &req) &&
      req.fd >= 0) {
    struct gpiohandle_data data = { };
    ioctl(req.fd, GPIOHANDLE_GET_LINE_VALUES_IOCTL, &data);
    close(req.fd);
    DBG("Relay::get(" << pin << ") -> " << (data.values[0] ? "true" : "false"));
    return data.values[0];
  }
  DBG("Relay::get(" << pin << ") -> ERROR");
  return false;
}

void Relay::toggle(int pin) {
  // There are two types of devices that we are trying to drive. For
  // keyfob-style remote controls, we turn the digital output to on. This
  // turns the output to 3.3V. It'll be read by a high-Ohm input on the
  // keyfob and interpreted as a button push.
  // For relay boards, the logic is reverse. A 3.3V signal will be
  // interpreted as off, and a 0V signal will be interpreted as on.
  // Ideally, we want the same code to work for both devices without having
  // to hard-code the type of device.
  // Fortunately though, the input is relatively low-Ohm (~1kΩ). When the
  // pin is configured for Gpio.INPUT it doesn't actually read as 0V, even
  // when the pull-down resistor is activated.
  // This allows us to have a common way to signal "off". Set the pin to
  // input and activate the pull-down resistor. For "on", we have to try
  // both outputting 3.3V and 0V, though. We only need to hold the value
  // for a short amount of time, when simulating button presses, though.
  set(pin, false);
  // The relay board now reads an "on" condition, but the keyfob is still "off"
  event_.addTimeout(300, [this, pin]() {
    // Next, we also signal an "on" condition to the keyfob remote. The relay
    // board will treat this as "off".
    set(pin, true);
    event_.addTimeout(300, [this, pin]() {
      // Return pin to the "off" condition, by relying on the pull-down resistor
      // to do the right thing for both types of devices.
      get(pin);
  }); });
}
