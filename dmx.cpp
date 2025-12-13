#include <fcntl.h>
#include <fmt/format.h> // Include fmt for the debug string
#include <iostream>
#include <math.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "dmx.h"
#include "serial.h"
#include "util.h"

#if !defined(NDEBUG)
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
static const char *dmxsrv;
#endif


DMX::DMX(Event& event, const std::string& dev)
  : event_(event), dev_(dev.empty() ? "/dev/ttyUSB0" : dev), fd_(-1),
    refreshTmo_(0) {
#if !defined(NDEBUG)
  // It is easier to develop on a more powerful device. Setting the
  // DMXSERVER environment variable to an empty string enables a server that
  // listens for UDP packages containing DMX records that need to be output.
  // Setting the same environment variable to an IPv4 address forwards all
  // requests over the network instead of talking to a locally connected
  // device.  dmxsrv = getenv("DMXSERVER");
  if (dmxsrv && !*dmxsrv) {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd >= 0) {
      struct sockaddr_in srv = { .sin_family = AF_INET,
                                 .sin_port = htons(53141) };
      srv.sin_addr.s_addr = INADDR_ANY;
      if (bind(fd, (const struct sockaddr *)&srv, sizeof(srv)) < 0) {
        close(fd);
      } else {
        event_.addPollFd(fd, POLLIN, [=, this](auto) {
          static int c = 0; if (write(1, &"-\\|/"[++c%4], 1)+write(1,"\010",1));
          unsigned char buf[513];
          auto n = recv(fd, buf, sizeof(buf), MSG_DONTWAIT);
          if (n > 0 && n <= (ssize_t)sizeof(buf)) {
            while (n--) {
              set(n, buf[n]);
            }
          }
          return true;
        });
      }
    }
  }
#endif
}

DMX::~DMX() {
  if (fd_ >= 0) {
    close(fd_);
    fd_ = -1;
  }
}

void DMX::set(int idx, int val, bool fade) {
  // We should always send at least 24 light levels in a DMX package. This
  // minimum package size ensures that we don't exceed DMX timing parameters.
  val = std::min(std::max(0, val), 255);
  if (idx <= 0 || idx > 512) return;
  if ((int)values_.size() <= std::max(23, idx)) {
    values_.resize(std::max(24, idx + 1), 0);
    phys_.resize(std::max(24, idx + 1), 0);
    adj_.resize(std::max(24, idx + 1), 0);
    fadeTime_.resize(std::max(24, idx + 1), 0);
  }
  // If we are setting a duplicate value, don't do anything else, now.
  if (values_[idx] == val) {
    return;
  }
  DBG("DMX::set(" << idx << ", " << val << ", " << (fade?"true":"false") <<")");

  // Some DMX controllers don't like being turned all the way on without
  // giving them a little time to ramp up. Also, slowly fading the lights
  // looks nicer. We keep track of the desired nominal output level in
  // "values_", but slowly approach this number by adjusting the "phys_"
  // setting.
  fadeTime_[idx] = std::max(1.0, FADE_TMO*std::abs(values_[idx] - val)/255.0);
  DBG("fadeTime = " << fadeTime_[idx]);
  values_[idx] = val;

  if (!fade
#if !defined(NDEBUG)
      || (dmxsrv && !*dmxsrv && abs(phys_[idx] - val) <= 30)
 #endif
      ) {
    phys_[idx] = val;
    adj_[idx] = 0;
  } else {
    // Ensure we don't accidentally set 0 (the inactive flag)
    unsigned now = Util::millis();
    adj_[idx] = now ? now - 5 : 1;
    if ((int)fadeFrom_.size() <= idx)
      fadeFrom_.resize(idx + 1, 0);
    fadeFrom_[idx] = phys_[idx];
  }

  // If the same parameter was updated more than once without the data being
  // sent to the light fixture, force an immediate update. This helps with
  // smooth fading.
  if ((int)updates_.size() <= idx) {
    updates_.resize(idx + 1, 0);
  }
  refresh(updates_[idx]++ ? 0 : 5);
}

void DMX::refresh(unsigned when) {
  // Wait until a break of at least 5ms before actually sending an updated
  // package. This allows a sequence of updates to all be made atomically.
  // Afterwards, switch to a regular low-frequency stream of DMX packages to
  // keep all the lights active, even if they were temporarily disconnected.
  event_.removeTimeout(refreshTmo_);
  // DBG("DMX::refresh(" << when << ")");
  if (!when) {
    sendPacket();
  } else {
    refreshTmo_ = event_.addTimeout(when, [this]() { sendPacket(); });
  }
}

void DMX::sendPacket() {
  // Send a full DMX update a couple of times per second. Send it more
  // frequently when values are actively changing.
  int nextTmo = 200;
  bool fading = false;

  for (unsigned i = 0; i < phys_.size(); ++i) {
    if (i < adj_.size() && adj_[i]) {
        double t = std::min(1.0,
                            (Util::millis() - adj_[i])/(double)fadeTime_[i]);
        fading = true;
        double curve = values_[i] > fadeFrom_[i] ? 0.1 : 0.2;
        phys_[i] =
          std::max(0, std::min(255, fadeFrom_[i] +
          (int)round(pow(t, curve)*(values_[i] - fadeFrom_[i]))));

        if (t >= 1.0) {
          adj_[i] = 0;
          phys_[i] = values_[i];
          // Note: 'fading' remains true for this iteration, which ensures
          // we schedule a quick follow-up to finalize the state.
        }
    } else if (phys_[i] != values_[i]) {
        phys_[i] = values_[i];
    }
  }

  if (fading) {
    nextTmo = 5;
  }

#if !defined(NDEBUG)
  // In remote mode, we send all DMX records by UDP to our server, instead of
  // directly talking to a serial device.
  if (dmxsrv && *dmxsrv) {
    struct sockaddr_in srv = { .sin_family = AF_INET,
                               .sin_port = htons(53141) };
    inet_aton(dmxsrv, &srv.sin_addr);
    static int fd = socket(AF_INET, SOCK_DGRAM, 0);
    sendto(fd, phys_.data(), phys_.size(), MSG_DONTWAIT,
           (const struct sockaddr *)&srv, sizeof(srv));
  } else
#endif
  {
    // Finally, write DMX data over RS485. Hide the details for setting up
    // the correct serial port parameters in the "Serial" object.
    if (fd_ < 0) {
      fd_ = Serial::open(dev_.c_str());
      if (fd_ < 0) {
        return;
      }
    }
    Serial::brk(fd_);
    if (write(fd_, phys_.data(), phys_.size()) != (ssize_t)phys_.size()) {
      DBG("Write error on serial port");
      close(fd_);
      fd_ = -1;
    }
    // std::string state("DMX:");
    // for (size_t i = 0; i < phys_.size(); ++i) {
    //     state += fmt::format(" {:3}", phys_[i]);
    // }
    // DBG(state);
  }

  // Clear the flags that show which parameters have changed. Then schedule
  // another update in 200ms. This can happen sooner, if there are any new
  // updates.
  updates_.clear();
  refresh(nextTmo);
}
