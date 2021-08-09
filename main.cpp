#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <sys/wait.h>
#include <unistd.h>

#include <fmt/format.h>
#include <fstream>
#include <iomanip>

#include "json.hpp"
using json = nlohmann::json;

#include "dmx.h"
#include "event.h"
#include "radiora2.h"
#include "relay.h"
#include "util.h"
#include "ws.h"


// Monitor the health of our process by sending regular heart beats. If the
// heartbeats stop, kill the child process and restart it from the parent.
static int childFd[2] = { -1, -1 };


static void setDMX(DMX& dmx, const json& dimmer, int level) {
  // Apply a dimmer curve and low trim level. Also, fade the color temperature.
  unsigned offset = !!(dimmer.size() > 0 && dimmer[0].is_number());
  const auto& ids = dimmer.size() > offset ? dimmer[offset] : "[]"_json;
  const auto& curve = dimmer.size() > offset+1 ? dimmer[offset+1] : "[]"_json;
  const auto& trim = dimmer.size() > offset+2 ? dimmer[offset+2] : "0"_json;
  for (unsigned i = 0; i < (ids.is_array() ? ids.size() : 0); i++) {
    if (!ids[i].is_number()) continue;
    int id = ids[i].get<int>();
    if (id <= 0 || id > 512) continue;
    double exp = curve.is_array() && curve.size() > i && curve[i].is_number()
                 ? curve[i].get<double>() : 1.0;
    double t = trim.is_number() ? trim.get<double>() : 0;
    int v = pow((level*(100.0-t)/100.0+t)/10000, exp)*255;
    dmx.set(id, v);
  }
}

static void readConfig(const std::string& fname,
                       RadioRA2& ra2, DMX& dmx, Relay& relay) {
  // Out of the box, our code does not implement any policy and won't really
  // change the behavior of the Lutron device. But given a "site.json"
  // configuration file, it can integrate non-Lutron devices into the
  // existing RadioRA2 system.
  std::ifstream ifs(fname);
  if ((ifs.rdstate() & std::ifstream::failbit) != 0) {
    DBG("Failed to read \"" << fname << "\"");
  } else {
    json site = json::parse(ifs, nullptr, false, true);
    if (site.is_discarded()) {
      DBG("Failed to parse \"" << fname << "\"");
    } else {
      // Iterate over all "DMX" object definitions and add virtual outputs
      // for DMX fixtures that are represented by dummy objects in the
      // Lutron system.
      const auto& dmxIds = site["DMX"];
      for (const auto& [name, params] : dmxIds.items()) {
        if (params.size() <= 0 || !params[0].is_number()) {
          continue;
        }
        ra2.addOutput(
          fmt::format("{}{}", RadioRA2::DMXALIAS, params[0].get<int>()),
          [&dmx, params](int level) {
            setDMX(dmx, params, level);
          });
      }
      // Iterate over all "KEYPAD" object definitions and add new assignments
      // to the various keypad buttons.
      const auto& keypad = site["KEYPAD"];
      for (const auto& [kp, buttons] : keypad.items()) {
        for (const auto& [bt, actions] : buttons.items()) {
          for (const auto& [at, rule] : actions.items()) {
            if (at == "DMX") {
              // Register DMX light fixtures with the "RadioRA2" object.
              // This is the most fundamental feature that we implement. It
              // makes DMX light fixtures behave just the same as native
              // Lutron output devices.
              for (const auto& [output, level] : rule.items()) {
                const auto& dmxIds = site["DMX"];
                const auto& params = dmxIds.find(output);
                if (params == dmxIds.end()) {
                  DBG("Cannot find DMX fixture \"" << output << "\"");
                  continue;
                }
                ra2.addToButton(
                  atoi(kp.c_str()), atoi(bt.c_str()),
                  ra2.addOutput(output,
                    [&, dimmer = *params](int level) {
                      setDMX(dmx, dimmer, level);
                    }),
                  level.get<int>());
              }
            } else if (at == "TOGGLE") {
              // Some devices (e.g. Pico remote) have artificial constraints,
              // forcing a button to enable a scene instead of allowing it to
              // be a toggle button. For these buttons, we don't assign any
              // fixtures in the Lutron controller and instead implement the
              // toggle function ourselves. This works by aliasing the physical
              // output device to a virtual copy that can be attached to a
              // callback.
              for (const auto& out : rule) {
                ra2.addToButton(
                  atoi(kp.c_str()), atoi(bt.c_str()),
                  ra2.addOutput(
                    fmt::format("{}{}", RadioRA2::ALIAS, out.get<int>()),
                    [&, out](int level) {
                      ra2.command(fmt::format(
                        "#OUTPUT,{},1,{}.{:02}",
                        out.get<int>(), level/100, level%100));
                    }), 100, true);
              }
            } else if (at == "DEVICE") {
              // An alternative way to achieve a similar goal is for the
              // Pico remote to simulate a button press on a different keypad.
              const int otherKp = rule[0].get<int>();
              const int otherBt = rule[1].get<int>();
              ra2.addToButton(
                atoi(kp.c_str()), atoi(bt.c_str()),
                ra2.addOutput(fmt::format("DEV:{}/{}", otherKp, otherBt),
                  [&, otherKp, otherBt](auto) {
                    ra2.command(fmt::format("#DEVICE,{},{},3",otherKp,otherBt));
                    ra2.command(fmt::format("#DEVICE,{},{},4",otherKp,otherBt));
                  }), 0);
            } else if (at == "RELAY") {
              // We can control GPIO inputs and outputs that frequently have
              // relays attached. Currently, only momentary push buttons are
              // implemented for output pins. But that could be extended as
              // needed.
              // A GPIO rule is a two-element vector that specifies a
              // prerequisite condition (i.e. a GPIO input), and an action to
              // take (i.e. a GPIO output). The condition can be omitted by
              // using an empty string. And it can be inverted by either
              // preceding the GPIO definition or the rule definition with a
              // "!".
              auto cond = rule[0].get<std::string>();
              const auto& action = rule[1].get<std::string>();
              bool sense = !(cond.size() > 0 && cond[0] == '!');
              if (!sense) {
                // Invert the sense of the GPIO input for this rule only.
                cond.erase(0, 1);
              }
              const auto& gpio = site["GPIO"];
              int condPin = -1;
              if (!cond.empty()) {
                // Globally invert the sense of this GPIO pin. This becomes
                // more complicated, as the JSON implementation doesn't fully
                // behave like STL containers, so we can't use std::find_if().
                for (auto& [k,v] : gpio.items()) {
                  if (k.rfind(cond, k[0] == '!') <= 1) {
                    condPin = v; sense ^= k[0] == '!'; break;
                  }
                }
              }
              // Also look up the output pin that should be toggled.
              const auto actionPin = gpio.find(action);
              // If we were able to successfully parse the GPIO rule, set up
              // a callback function that will be invoked any time the user
              // pushes a button on the keypad.
              if ((cond.empty() || condPin >= 0) && actionPin != gpio.end()) {
                ra2.addToButton(
                  atoi(kp.c_str()), atoi(bt.c_str()),
                  ra2.addOutput(fmt::format("RELAY:{}/{}",
                    condPin, actionPin->get<int>()),
                    [&, sense, condPin,
                     actionPin = actionPin->get<int>()](auto) {
                      if (condPin < 0 || relay.get(condPin) == sense) {
                        relay.toggle(actionPin);
                      }
                    }), 0);
              } else {
                DBG("Cannot parse GPIO rule");
              }
            } else {
              DBG("Unknown event type: " << at);
            }
          }
        }
      }
    }
  }
}

static void dmxRemoteServer(Event& event) {
#ifndef NDEBUG
  // By setting the DMXSERVER environment variable to an empty string, we
  // become a proxy for DMX requests.
  const char *dmxsrv = getenv("DMXSERVER");
  if (dmxsrv && !*dmxsrv) {
    DBG("Running in remote server mode");
    DMX dmx(event);
    event.loop();
    exit(0);
  }
#endif
}

static void server() {
  // Create all the different objects that make up our server and connect
  // them to each other. Then enter the event loop.
  Event event;
  WS ws(&event, 8080);
  dmxRemoteServer(event);
  DBG("Starting...");
  DMX dmx(event);
  Relay relay(event);
  RadioRA2 ra2(event,
               [&]() { readConfig("site.json", ra2, dmx, relay); },
               nullptr,
               // Communicate with parent process. This allows the watchdog
               // to kill us, if we become unresponsive. And it also allows
               // us to request a restart, if the automation schema changed
               // unexpectedly.
               [](){ if (childFd[1] >= 0 && write(childFd[1], "", 1));},
               [](){ if (childFd[1] < 0 || !write(childFd[1], "\1", 1)) {
                    DBG("Stale cached data"); _exit(1);}});
  event.loop();
}

int main(int argc, char *argv[]) {
#if defined(NDEBUG)
  // In production mode, wrap server with a helper process that restarts in
  // case of unexpected crashes, missed heartbeat signals, or when the
  // schema changes. In debug mode, disable this feature, as it is much
  // easier to attach a debugger to a single-process application.
  for (;;) {
    // Communication pipe between parent and child process.
    if (childFd[0] >= 0) close(childFd[0]);
    if (pipe2(childFd, O_CLOEXEC | O_NONBLOCK)) {
      return 1;
    }
    const auto p = fork();
    if (p == 0) {
      close(childFd[0]);
      server();
      exit(0);
    } else if (p > 0) {
      // In the parent process, implement a watchdog timer that kills and
      // restarts the child, if we don't see a regular heartbeat.
      close(childFd[1]);
      Event event;
      bool restart = false;
      void *tmo = nullptr;
      const auto resetTmo = [&]() {
        event.removeTimeout(tmo);
        tmo = event.addTimeout(120*1000, [&]() {
          restart = true;
          kill(p, SIGKILL);
        });
      };
      resetTmo();
      // Use the event loop to keep track of heartbeats, requests for restart,
      // and process termination (resulting in an EOF condition on the pipe).
      for (;;) {
        event.removePollFd(childFd[0]);
        event.addPollFd(childFd[0], POLLIN, [&]() {
          char ch = 0;
          ssize_t i = read(childFd[0], &ch, 1);
          if (i == 1 || (i < 0 && (errno == EAGAIN || errno == EINTR))) {
            if (ch) {
              // Child requested to be restarted. This typically happens because
              // the Lutron device changed the automation schema, but we
              // already started setting up our internal data structure with
              // a schema that is now out of date. A full restart is the
              // easiest solution to get back into a defined state.
              kill(p, SIGKILL);
              restart = true;
            } else {
              // We received a heartbeat signal. Tell the watchdog timer that
              // everything is still OK.
              resetTmo();
              return true;
            }
          }
          // If the child exited, leave the event loop and use waitpid() to
          // check what needs to be done.
          event.exitLoop();
          return false;
        });
        event.loop();
        int status = 0;
        const auto rc = waitpid(p, &status, 0);
        if (rc < 0) {
          // If waitpid() failed unexpectedly, kill the child process.
          // Something went wrong, and we should just terminate altogether.
          if (errno == EINTR) { continue; }
          if (errno != ECHILD) { kill(p, SIGKILL); }
          return 1;
        }
        if (WIFEXITED(status) && !WEXITSTATUS(status)) {
          // If the child terminated normally, then so should we.
          return 0;
        } else if (restart || !WIFSIGNALED(status) || WCOREDUMP(status)) {
          // If the child requested to be restarted, or if it crashed
          // unexpectedly, start a new instance.
          break;
        } else {
          // Any other signal suggests that the child was told to quit
          // (e.g. the user pressed CTRL-C). Exit now.
          return 1;
        }
      }
    } else {
      // If we failed to fork() a child process, there isn't much else we
      // can do.
      return 1;
    }
  }
#else
  server();
#endif
  return 0;
}
