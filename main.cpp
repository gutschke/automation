#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <signal.h>
#include <sys/signalfd.h>
#include <sys/wait.h>
#include <unistd.h>

#include <fmt/format.h>
#include <fstream>
#include <iomanip>
#include <set>

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
static bool initialized = false;


static void setDMX(DMX& dmx, const json& dimmer, int level, bool fade) {
  // Apply a dimmer curve and low trim level. Also, fade the color temperature.
  static std::map<int, int> early;
  if (initialized && !early.empty()) {
    for (const auto& [id, v] : early) {
      dmx.set(id, v, false);
    }
    early.clear();
  }
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
    if (initialized) {
      dmx.set(id, v, fade);
    } else {
      early[id] = v;
    }
  }
}

static void readLine(RadioRA2& ra2, DMX& dmx, Relay& relay,
                     const std::string& line, const std::string& context,
                     bool fade) {
  DBG("readLine(\"" << line << "\", \"" << context << "\")");
  if (Util::starts_with(line, "~OUTPUT,")) {
    // When an output device changes levels, we expect a line of the form
    // "~OUTPUT,<dev>,1,<level>". If this was a dummy device that stands in for
    // a DMX load, the user can specify the DMX info in the device name.
    // This makes the "site.json" file unnecessary and allows the user to
    // design their entire system inside of the Lutron software.
    // The additional information needed is provided to us in the "context".
    // A ":" after the device name includes the JSON string that we
    // subsequently need to pass to setDMX().
    auto comma = strchr(&line[8], ',');
    if (!memcmp(",1,", comma, 3)) {
      // Check whether the "context" references a DMX load.
      auto args = context.find(':');
      if (args != std::string::npos) {
        // Lutron outputs the level as a number in the range 0..100 with two
        // decimals precision. Convert it to an integer in the range 0..10000.
        int level = 100*atoi(comma + 3);
        auto decimal = strchr(comma + 3, '.');
        if (decimal && decimal[1] >= '0' && decimal[1] <= '9') {
          level += 10*(decimal[1] - '0');
          if (decimal[2] >= '0' && decimal[2] <= '9') {
            level += decimal[2] - '0';
          }
        }
        if (context[args + 1] == '[') {
          DBG("Found in-line DMX info");
          setDMX(dmx, json::parse("[" + context.substr(args + 1) + "]"),
                 std::min(std::max(0, level), 10000), fade);
        } else if (initialized) {
          // Some dimmers are supposed to be darker at night and brighter
          // during the day. A ":<low>/<high>/<from>-<to>" parameter can
          // override the Lutron defaults.
          char *endptr;
          errno = 0;
          auto low  = strtol(&context[args + 1], &endptr, 0);
          auto hi   = strtol(*endptr ? endptr + 1 : "", &endptr, 0);
          auto from = strtol(*endptr ? endptr + 1 : "", &endptr, 0);
          auto to   = strtol(*endptr ? endptr + 1 : "", &endptr, 0);
          if (!errno && low >= 0 && low <= 100 && hi >= 0 && hi <= 100 &&
              from >= 0 && from <= 2400 && to >= 0 && to <= 2400 &&
              level > 150 && abs(level - hi*100) > 250 &&
              (abs(level - low*100) < 200 || level - 750 < low*100)) {
            int now = Util::timeOfDay();
            if ((now >= from && now < to) == (to > from)) {
              static std::map<int, int> suppress;
              int id = atoi(std::string(line, 8, comma-&line[8]).c_str());
              const auto it = suppress.find(id);
              if (it == suppress.end() || Util::millis() - it->second > 2000) {
                ra2.command(fmt::format("#OUTPUT,{},1,{}.00", id, hi));
              }
              suppress[id] = Util::millis();
            }
          }
        }
      }
    }
  } else if (Util::starts_with(line, "~DEVICE,") &&
             Util::ends_with(line, ",3")) {
    const auto dev = atoi(&line[8]);
    // Pico remotes aren't output devices, but we can track their buttons and
    // make them behave like virtual key events for a keypad. Again, this
    // information can be encoded using the Pico label.
    // Button presses will be reported with a line of the form
    // "~DEVICE,<pico>,<button>,3".
    // We also look at keypads, and interpret any in-line information as
    // instructions when to toggle relay outputs.
    auto args = context.find(':');
    if (args != std::string::npos) {
      switch (ra2.deviceType(dev)) {
      case RadioRA2::DEV_PICO_KEYPAD: {
        auto json = json::parse("[" + context.substr(args + 1) + "]");
        switch (json.size()) {
        case 1: // This button controls an output and behaves like "TOGGLE"
                // directive in a "site.json" file.
          ra2.toggleOutput(json[0].get<int>());
          break;
        case 2:{// This button forwards the button press to a different keypad,
                // and behaves like the "DEVICE" directive in a "site.json" file.
          const int otherKp = json[0].get<int>();
          const int otherBt = json[1].get<int>();
          ra2.command(fmt::format("#DEVICE,{},{},3", otherKp, otherBt));
          ra2.command(fmt::format("#DEVICE,{},{},4", otherKp, otherBt));
          break; }
        default:
          break;
        }
        break; }
      case RadioRA2::DEV_SEETOUCH_KEYPAD:
      case RadioRA2::DEV_HYBRID_SEETOUCH_KEYPAD: {
        std::string cond = Util::trim(context.substr(args + 1));
        const bool sense = !(cond.size() > 0 && cond[0] == '!');
        if (!sense) {
          cond.erase(0, 1);
        }
        auto comma = cond.find(',');
        int condPin = -1;
        if (comma != std::string::npos) {
          condPin = atoi(cond.c_str());
          cond = Util::trim(cond.substr(comma + 1));
        }
        char *endptr;
        int actionPin = (int)strtoul(cond.c_str(), &endptr, 10);
        if (condPin < 0 || relay.get(condPin) == sense) {
          bool slow = false;
          for (; *endptr; ++endptr) slow |= (*endptr == 'S');
          relay.toggle(actionPin, slow);
        }
        break; }
      default:
        break;
      }
    }
  }
}

static void runScript(Event& event, RadioRA2& ra2, const std::string& script) {
  ra2.updateEnvironment();

  // Create a non-blocking pipe for the script's output
  int pipefd[2];
  if (pipe2(pipefd, O_CLOEXEC | O_NONBLOCK)) {
    DBG("Failed to create pipe");
    return;
  }

  pid_t pid = fork();
  if (pid == 0) {
    // Child process
    close(pipefd[0]);    // Close the read end
    dup2(pipefd[1], 1);  // Redirect stdout to the pipe
    close(pipefd[1]);    // Close the original write end

    // Unblock signals! The daemon blocked them, but the script
    // needs them to function correctly (e.g. ctrl+c, SIGTERM).
    sigset_t mask;
    sigemptyset(&mask);
    sigprocmask(SIG_SETMASK, &mask, nullptr);

    // Replace process with the shell
    execl("/bin/sh", "sh", "-c", script.c_str(), (char *)nullptr);
    _exit(127); // If exec fails
  } else if (pid > 0) {
    // Parent process
    close(pipefd[1]); // Close the write end so we get EOF when child is done

    // Monitor the pipe for output asynchronously
    event.addPollFd(pipefd[0], POLLIN,
      [&ra2, fd = pipefd[0], buf = std::string()](pollfd *pfd) mutable {
      char buffer[1024];
      ssize_t rc = read(fd, buffer, sizeof(buffer));

      if (rc > 0) {
        // Data received: Buffer it and extract complete lines
        buf.append(buffer, rc);
        size_t pos;
        while ((pos = buf.find('\n')) != std::string::npos) {
          std::string line = Util::trim(buf.substr(0, pos));
          if (!line.empty()) {
            ra2.command(line);
          }
          buf.erase(0, pos + 1);
        }
        return true;
      } else if (rc < 0 &&
                 (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
        // No data ready, keep listening
        return true;
      } else {
        // EOF or error. The script finished or crashed. Process any final
        // lingering data.
        std::string line = Util::trim(buf);
        if (!line.empty()) {
          ra2.command(line);
        }

        // Stop monitoring this FD. Note that we do not call waitpid() here.
        // The main loop handles SIGCHLD.
        close(fd);
        return false;
      }
    });
  } else {
    close(pipefd[0]);
    close(pipefd[1]);
    DBG("Failed to fork script process");
  }
}

static void augmentConfig(Event& event, const json& site, RadioRA2& ra2,
                          DMX& dmx, Relay& relay) {
  // Out of the box, our code does not implement any policy and won't really
  // change the behavior of the Lutron device. But given a "site.json"
  // configuration file, it can integrate non-Lutron devices into the
  // existing RadioRA2 system.

  // Iterate over all "DMX" object definitions and add virtual outputs
  // for DMX fixtures that are represented by dummy objects in the
  // Lutron system.
  if (site.contains("DMX")) {
    const auto& dmxIds = site["DMX"];
    for (const auto& [name, params] : dmxIds.items()) {
      if (!params.is_array() || params.size() <= 0 || !params[0].is_number()) {
        continue;
      }
      ra2.addOutput(
        fmt::format("{}{}", RadioRA2::DMXALIAS, params[0].get<int>()),
        [&dmx, params](int level, bool fade) {
          setDMX(dmx, params, level, fade);
        });
    }
  }
  // Iterate over all "WATCH" objects and attach actions that should
  // trigger when an output changes.
  if (site.contains("WATCH")) {
    const auto& watch = site["WATCH"];
    for (const auto& [id_, script] : watch.items()) {
      if (id_ == "TIMECLOCK") {
        ra2.monitorTimeclock([&event, &ra2, &script](const std::string& s) {
            unsetenv("KEYPAD");
            unsetenv("BUTTON");
            unsetenv("ON");
            unsetenv("LONG");
            unsetenv("NUMTAPS");
            unsetenv("OUTPUT");
            unsetenv("LEVEL");
            unsetenv("level");
            setenv("TIMECLOCK", s.c_str(), 1);
            runScript(event, ra2, script);
          });
      } else {
        const auto id = atoi(id_.c_str());
        ra2.monitorOutput(id, [id, &event, &ra2, &script](int level) {
            unsetenv("KEYPAD");
            unsetenv("BUTTON");
            unsetenv("ON");
            unsetenv("LONG");
            unsetenv("NUMTAPS");
            unsetenv("TIMECLOCK");
            setenv("OUTPUT", fmt::format("{}", id).c_str(), 1);
            setenv("LEVEL",
                   fmt::format("{}.{:02}", level/100, level%100).c_str(), 1);
            setenv("level", fmt::format("{}", level).c_str(), 1);
            runScript(event, ra2, script);
          });
      }
    }
  }
  // The I2C object allows us to define virtual GPIO pins that need to be
  // addressed through an I2C bus instead.
  if (site.contains("I2C")) {
    const auto& i2c = site["I2C"];
    for (const auto& [id_, def] : i2c.items()) {
      const auto id = atoi(id_.c_str());
      relay.i2c(id, def["BUS"].get<int>(), def["DEV"].get<int>(),
                def["ADDR"].get<int>(), def["BIT"].get<int>());
    }
  }
  // Iterate over all "KEYPAD" object definitions and add new assignments
  // to the various keypad buttons.
  if (site.contains("KEYPAD")) {
    const auto& keypad = site["KEYPAD"];
    for (const auto& [kp, buttons] : keypad.items()) {
      for (const auto& [bt, actions] : buttons.items()) {
        for (const auto& [at, rule] : actions.items()) {
          if (at == "DMX" && site.contains("DMX")) {
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
                  [&, dimmer = *params](int level, bool fade) {
                    setDMX(dmx, dimmer, level, fade);
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
                  [&, out](int level, auto) {
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
                [&, otherKp, otherBt](auto, auto) {
                  ra2.command(fmt::format("#DEVICE,{},{},3",otherKp,otherBt));
                  ra2.command(fmt::format("#DEVICE,{},{},4",otherKp,otherBt));
                }), 0);
          } else if (at == "SCRIPT") {
            // Sometimes, none of the built-in rules can do the job. Branch out
            // to a helper script instead.
            auto script = rule.get<std::string>();
            if (!script.empty()) {
              ra2.addButtonListener(
                atoi(kp.c_str()), atoi(bt.c_str()),
                [script, &event, &ra2](
                     int kp, int bt, bool on, bool isLong, int num) {
                  unsetenv("TIMECLOCK");
                  unsetenv("OUTPUT");
                  unsetenv("LEVEL");
                  setenv("KEYPAD",  fmt::format("{}", kp).c_str(), 1);
                  setenv("BUTTON",  fmt::format("{}", bt).c_str(), 1);
                  setenv("ON",      fmt::format("{}", on).c_str(), 1);
                  if (isLong) setenv("LONG", "1", 1);
                  else      unsetenv("LONG");
                  if (num) setenv("NUMTAPS", fmt::format("{}", num).c_str(), 1);
                  else   unsetenv("NUMTAPS");
                  runScript(event, ra2, script);
                });
            }
          } else if (at == "RELAY" && site.contains("GPIO")) {
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
            // Also look up the output pin that should be toggled. We detect
            // any flags that might be present.
            int actionPin = -1;
            bool slow = false;
            for (auto& [k,v] : gpio.items()) {
              auto flags = std::find(k.begin(), k.end(), '/');
              if (std::string(k.begin(), flags) == action) {
                actionPin = v.get<int>();
                for (; flags != k.end(); ++flags) {
                  slow |= (*flags == 'S');
                }
                break;
              }
            }
            // If we were able to successfully parse the GPIO rule, set up
            // a callback function that will be invoked any time the user
            // pushes a button on the keypad.
            if ((cond.empty() || condPin >= 0) && actionPin >= 0) {
              ra2.addToButton(
                atoi(kp.c_str()), atoi(bt.c_str()),
                ra2.addOutput(fmt::format("RELAY:{}/{}", condPin, actionPin),
                  [&, sense, condPin, actionPin, slow](auto, auto) {
                    if (condPin < 0 || relay.get(condPin) == sense) {
                      relay.toggle(actionPin, slow);
                    }
                  }), -1);
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

static void updateUI(WS* ws, Event& event, int kp, int led,
                     bool state, int level) {
  if (!ws) {
    return;
  }
  static std::map<std::pair<int, int>, std::pair<bool, int>> cache;
  if (!cache.size()) {
    // Batch multiple updates into a single broadcast message.
    event.addTimeout(100, [ws]() {
      std::string s;
      for (const auto& [ k, v ] : cache) {
        s += fmt::format("{},{},{},{}.{:02} ",
               k.first, k.second, (int)v.first, v.second/100, v.second%100);
      }
      s.pop_back();
      cache.clear();
      ws->broadcast(s);
    });
  }
  cache[std::make_pair(kp, led)] = std::make_pair(state, level);
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

static std::vector<int> keypadOrder(const json& site, const RadioRA2& ra2) {
  // The "KEYPAD ORDER" parameter is optional and sets a prefered display
  // order for the keypads in the web UI.
  std::vector<int> order;
  if (site.contains("KEYPAD ORDER")) {
    for (const auto& kp : site["KEYPAD ORDER"]) {
      if (kp.is_string()) {
        int id = ra2.getKeypad(kp.get<std::string>());
        if (id >= 0) {
          order.push_back(id);
        }
      } else if (kp.is_number()) {
        order.push_back(kp.get<int>());
      }
    }
  }
  return order;
}

static void server() {
  // Read the "site.json" file, if present. Some of the data will be needed
  // early to initialize global state. Other data will be used at a later point
  // to augment the information that we retrieve from the Lutron controller.
  json site("{}"_json);
  const std::string& fname = "site.json";
  {
    std::ifstream ifs(fname);
    if ((ifs.rdstate() & std::ifstream::failbit) != 0) {
      DBG("Failed to read \"" << fname << "\"");
    } else {
      json cfg = json::parse(ifs, nullptr, false, true);
      if (cfg.is_discarded()) {
        DBG("Failed to parse \"" << fname << "\"");
      } else {
        site = std::move(cfg);
      }
    }
  }

  // Create all the different objects that make up our server and connect
  // them to each other. Then enter the event loop.
  Event event;

  // Block signals so they can be handled via the file descriptor
  sigset_t mask;
  sigemptyset(&mask);
  sigaddset(&mask, SIGTERM);
  sigaddset(&mask, SIGINT); // Handle ctrl+c gracefully too
  sigaddset(&mask, SIGCHLD);
  if (sigprocmask(SIG_BLOCK, &mask, nullptr) < 0) {
      DBG("Failed to block signals");
  }

  // Create a file descriptor for signals
  int sfd = signalfd(-1, &mask, SFD_NONBLOCK | SFD_CLOEXEC);
  if (sfd >= 0) {
    event.addPollFd(sfd, POLLIN, [&](pollfd* pfd) {
      struct signalfd_siginfo fdsi;
      if (read(sfd, &fdsi, sizeof(fdsi)) == sizeof(fdsi)) {
        if (fdsi.ssi_signo == SIGCHLD) {
          while (waitpid(-1, nullptr, WNOHANG) > 0) { }
        } else {
          DBG("Received signal " << fdsi.ssi_signo << ", exiting...");
          event.exitLoop(); // This triggers the graceful shutdown
        }
      }
      return true;
    });
  }

  dmxRemoteServer(event); // For debugging purposes only

  DBG("Starting...");
  DMX dmx(
    event,
    site.contains("DMX SERIAL") ? site["DMX SERIAL"].get<std::string>() : "");
  Relay relay(event);
  WS *ws = nullptr;
  RadioRA2 ra2(
    event, site.contains("REPEATER") ? site["REPEATER"].get<std::string>() : "",
    site.contains("USER") ? site["USER"].get<std::string>() : "",
    site.contains("PASSWORD") ? site["PASSWORD"].get<std::string>() : "");
  ra2.oninit([&]() {
    augmentConfig(event, site, ra2, dmx, relay); initialized = true;})
     .oninput([&](const std::string& line, const std::string&context,bool fade){
                readLine(ra2, dmx, relay, line, context, fade); })
     .onledstate([&](int kp, int led, bool state, int level) {
                   updateUI(ws, event, kp, led, state, level); })
     // Communicate with parent process. This allows the watchdog
     // to kill us, if we become unresponsive. And it also allows
     // us to request a restart, if the automation schema changed
     // unexpectedly.
     .onheartbeat([](){ if (childFd[1] >= 0 && write(childFd[1], "", 1));})
     .onschemainvalid([](){ if (childFd[1] < 0 || !write(childFd[1], "\1", 1)) {
           DBG("Stale cached data"); _exit(1);}});

  WS ws_(&event,
         site.contains("HTTP PORT") ? site["HTTP PORT"].get<int>() : 8080);
  ws_.onkeypadreq([&]() { return ra2.getKeypads(keypadOrder(site, ra2)); })
     .oncommand([&](const std::string& s) { ra2.command(s); });
  ws = &ws_;
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

      sigset_t mask;
      sigemptyset(&mask);
      sigaddset(&mask, SIGTERM);
      sigaddset(&mask, SIGINT);
      if (sigprocmask(SIGBLOCK, &mask, nullptr) < 0) {
        DBG("Failed to block signals in parent");
      }
      int sfd = signalfd(-1, &mask, SFD_NONBLOCK | SFD_CLOEXEC);
      if (sfd >= 0) {
        event.addPollFd(sfd, POLLIN, [&](pollfd *pfd) {
          struct signalfd_siginfo fdsi;
          if (read(sfd, &fdsi, sizeof(fdsi)) == sizeof(fdsi)) {
            if (fdsi.ssi_signo == SIGCHLD) {
              while (waitpid(-1, nullptr, WNOHANG) > 0) { }
            } else {
              // Forward signal (e.g. SIGTERM) and setup force-kill watchdog
              kill(p, fdsi.ssi_signo);

              // Schedule a forced SIGKILL if the child doesn't die within 5s
              event.addTimeout(5000, [p]() {
                DBG("Child stuck, forcing SIGKILL");
                kill(p, SIGKILL);
              });
              // Do not call event.exitLoop() here. We must keep the loop
              // running so the timeout above can fire.  When the child
              // eventually dies (gracefully or forced), the pipe childFd[0]
              // will close, triggering the loop exit elsewhere.
            }
          }
          return true;
        });
      }
      bool restart = false;
      void *tmo = nullptr;
      const auto resetTmo = [&]() {
        event.removeTimeout(tmo);
        tmo = event.addTimeout(120*1000, [&]() {
          restart = true;
          kill(p, SIGTERM);
          // Force kill, if it doesn't exit within 5 seconds
          event.addTimeout(5000, [p]() { kill(p, SIGKILL); });
        });
      };
      resetTmo();
      // Use the event loop to keep track of heartbeats, requests for restart,
      // and process termination (resulting in an EOF condition on the pipe).
      for (;;) {
        event.removePollFd(childFd[0]);
        event.addPollFd(childFd[0], POLLIN, [&](auto) {
          char ch = 0;
          ssize_t i = read(childFd[0], &ch, 1);
          if (i == 1 || (i < 0 && (errno == EAGAIN || errno == EINTR))) {
            if (ch) {
              // Child requested to be restarted. This typically happens because
              // the Lutron device changed the automation schema, but we
              // already started setting up our internal data structure with
              // a schema that is now out of date. A full restart is the
              // easiest solution to get back into a defined state.
              kill(p, SIGTERM);
              event.addTimeout(5000, [p]() { kill(p, SIGKILL); });
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
