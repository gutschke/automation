#include <fcntl.h>
#include <fmt/format.h>
#include <linux/gpio.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "relay.h"
#include "util.h"


Relay::Relay(Event& event, const std::string& deviceName)
  : event_(event),
    fd_(open(deviceName.c_str(), O_RDONLY)) {
}

Relay::~Relay() {
  for (const auto [ _, fd ] : handles_) {
    close(fd[1]);
  }
  if (fd_ >= 0) {
    close(fd_);
  }
}

int Relay::getHandle(int pin, int mode) {
  // If we can't access GPIO, then there is nothing to do here.
  if (fd_ < 0) {
    return -1;
  }
  // If the input or output mode has changed, we have to close the file
  // handle and get a new one.
  auto it = handles_.find(pin);
  if (it != handles_.end() &&
      (it->second[0] & (int)
       (GPIOHANDLE_REQUEST_INPUT | GPIOHANDLE_REQUEST_OUTPUT)) != mode) {
    close(it->second[1]);
    handles_.erase(it);
    it = handles_.end();
  }
  // If we don't yet have a handle, request one now.
  if (it == handles_.end()) {
    struct gpiohandle_request req = { };
    req.lineoffsets[0] = pin;
    req.lines = 1;
    req.flags = mode;
    if (!ioctl(fd_, GPIO_GET_LINEHANDLE_IOCTL, &req) && req.fd >= 0) {
      handles_.insert(std::make_pair(pin, std::array<int, 2>{mode, req.fd}));
      return req.fd;
    } else {
      return -1;
    }
  }
  return it->second[1];
}

void Relay::set(int pin, bool state, int bias) {
  DBG("Relay::set(" << pin << ", " << (state ? "true" : "false") << ")");
  const int handle = getHandle(pin, GPIOHANDLE_REQUEST_OUTPUT);
  if (handle >= 0) {
    struct gpiohandle_config conf = { };
    conf.flags = GPIOHANDLE_REQUEST_OUTPUT |
                 (bias == -1 ? GPIOHANDLE_REQUEST_BIAS_DISABLE : bias);
    // If the configuration flags (e.g. for pull up/down bias) have changed,
    // update the settings now.
    if (handles_[pin][0] != (int)conf.flags) {
      handles_[pin][0] = conf.flags;
      ioctl(handle, GPIOHANDLE_SET_CONFIG_IOCTL, &conf);
    }

    // Set the desired output state of the pin.
    struct gpiohandle_data data = { };
    data.values[0] = state ? 1 : 0;
    ioctl(handle, GPIOHANDLE_SET_LINE_VALUES_IOCTL, &data);
  }
}

bool Relay::get(int pin, int bias) {
  const int handle = getHandle(pin, GPIOHANDLE_REQUEST_INPUT);
  if (handle >= 0) {
    struct gpiohandle_config conf = { };
    conf.flags = GPIOHANDLE_REQUEST_INPUT |
                 (bias == -1 ? GPIOHANDLE_REQUEST_BIAS_PULL_UP : bias);
    // If the configuration flags (e.g. for pull up/down bias) have changed,
    // update the settings now.
    if (handles_[pin][0] != (int)conf.flags) {
      handles_[pin][0] = conf.flags;
      ioctl(handle, GPIOHANDLE_SET_CONFIG_IOCTL, &conf);
    }

    // Read the input pin.
    struct gpiohandle_data data = { };
    ioctl(handle, GPIOHANDLE_GET_LINE_VALUES_IOCTL, &data);
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
  set(pin, true, GPIOHANDLE_REQUEST_BIAS_DISABLE);

  // The relay board now reads an "on" condition, but the keyfob is still "off"
  event_.addTimeout(300, [this, pin]() {
    // Next, we also signal an "on" condition to the keyfob remote. The relay
    // board will treat this as "off".
    set(pin, false, GPIOHANDLE_REQUEST_BIAS_DISABLE);

    event_.addTimeout(300, [this, pin]() {
      // Return pin to the "off" condition, by relying on the pull-down resistor
      // to do the right thing for both types of devices.
      get(pin, GPIOHANDLE_REQUEST_BIAS_PULL_DOWN);
      }); });
}
