#include <arpa/inet.h>
#include <errno.h>
#include <locale.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include <fmt/format.h>
#include <iostream>
#include <sstream>

#include "lutron.h"
#include "radiora2.h"
#include "util.h"


RadioRA2::RadioRA2(Event& event, const std::string& gateway,
                   const std::string& username, const std::string& password)
  : event_(event),
    lutron_(event, gateway, username, password),
    initialized_(false),
    init_(),
    input_(nullptr),
    ledState_(nullptr),
    hb_(nullptr),
    schemaInvalid_(nullptr),
    recompute_(0),
    reconnect_(SHORT_REOPEN_TMO),
    checkStarted_(0),
    checkFinished_(0),
    uncertain_(0),
    schemaSock_(-1) {
  setlocale(LC_NUMERIC, "C");
  lutron_.oninit([this](auto cb) { init(cb); })
         .oninput([this](const std::string& line) { readLine(line); })
         .onclosed([this]() { closed(); });

  // The health check not only makes sure that we re-establish a connection
  // whenever it fails, but it also leaves a persistent object that keeps the
  // event loop from exiting.
  healthCheck();
}

RadioRA2::~RadioRA2() {
}

void RadioRA2::healthCheck() {
  // If the connection closed unexpectedly, retry opening it every so often.
  // If it already is open, verify that it still responds to regular commands.
  // If it doesn't, close it and open it again. That usually resets things.
  // This is a slightly higher-level health check than the very basic check
  // that the Lutron object performs by regularly sending a CRLF pair.
  if (!lutron_.isConnected()) {
    checkStarted_ = checkFinished_ = 0;
    lutron_.ping([this]() {
      reconnect_ = SHORT_REOPEN_TMO;
      checkFinished_ = Util::millis(); });
    reconnect_ = std::min(LONG_REOPEN_TMO, 2*reconnect_);
  } else {
    reconnect_ = SHORT_REOPEN_TMO;
    const auto now = Util::millis();
    if (checkStarted_ && (now - checkStarted_) > ALIVE_CMD_TMO) {
      lutron_.closeSock();
      checkStarted_ = checkFinished_ = 0;
    } else if (checkFinished_ &&
               (now - checkFinished_) > ALIVE_INTERVAL) {
      checkStarted_ = now;
      lutron_.ping([this]() {
        checkFinished_ = Util::millis();
        checkStarted_ = 0; });
    }
    if (!uncertain_) {
      uncertain_ = now;
    } else if (now - uncertain_ > 15*60*1000) {
      uncertain_ = now;
      for (const auto& [ _, dev ] : devices_) {
        for (const auto& [ _, btn ] : dev.components) {
          if (btn.uncertain) {
            command(fmt::format("#DEVICE,{},{},9,{}",
                                dev.id, btn.led, btn.ledState ? 1 : 0));
          }
        }
      }
    }
  }
  event_.addTimeout(reconnect_, [this]() { healthCheck(); });
}

void RadioRA2::readLine(const std::string& line) {
  if (hb_) {
    hb_();
  }
  if (!line.size()) {
    return;
  }
  DBGc(2, "Read line: \"" << line << "\"");
  bool suppressed = false;
  std::string context;
  // Received an update about a device. We are primarily interested in LEDs
  // and in light fixtures.
  if (Util::starts_with(line, "~DEVICE,")) {
    // We find out about LEDs with a line of the form
    //
    //   ~DEVICE,${IntegrationID},${ComponentNumber},9,${State}
    //
    // The state can be "0" for off, "1" for on, and anything else (e.g. 255)
    // for unknown.
    //
    // Similarly, we find out about button presses with a line of the form
    //
    //   ~DEVICE,${IntegrationID},${ComponentNumber},[3, 4]
    //
    // The former is a button down event, and the latter a button up.
    char *endPtr;
    // Find keypad that matches the ${IntegrationID}.
    const auto& dev = devices_.find((int)strtol(line.c_str() + 8, &endPtr, 10));
    if (dev != devices_.end() && *endPtr++ == ',') {
      int l = (int)strtol(endPtr, &endPtr, 10);
      auto& keypad = dev->second;
      if (keypad.components.find(l) != keypad.components.end()) {
        // This is a button and not an LED
        if (!strcmp(endPtr, ",3") || !strcmp(endPtr, ",4")) {
          auto& button = keypad.components[l];
          buttonPressed(keypad, button, endPtr[1] == '4');
          context = button.name;
        }
      } else {
        // Find LED that matches the ${ComponentNumber}. This is complicated
        // by the fact that in our internal data structures, we always match up
        // LEDs with their associated button.
        const auto& led = std::find_if(keypad.components.begin(),
                                       keypad.components.end(),
                                       [&](auto& t) -> bool {
                                         return t.second.led == l;
                                       });
        // If the rest of the command identifies a new LED state, update our
        // internal copy. If we only received "255", assume that the LED is
        // probably off. And that's the default state that the object's
        // constructor left it in. So, nothing to do here.
        if (led != keypad.components.end() && *endPtr++ == ',' &&
            *endPtr++ == (ACTION_LEDSTATE + '0') && *endPtr++ == ',') {
          context = led->second.name;
          led->second.uncertain = (*endPtr != '0' && *endPtr != '1')||endPtr[1];
          if (!led->second.uncertain) {
            if (ledState_ &&
                (keypad.type == DEV_SEETOUCH_KEYPAD ||
                 keypad.type == DEV_HYBRID_SEETOUCH_KEYPAD)) {
              const int level = getLevelForButton(led->second.assignments);
              ledState_(keypad.id, led->second.id,
                        !!(*endPtr == '1' || (*endPtr != '0' && level)),
                        level);
            }
            led->second.ledState = *endPtr == '1';
          }
        }
      }
    }
  } else if (Util::starts_with(line, "~OUTPUT,")) {
    // We find out about light fixtures with a line of the form
    //
    //   ~OUTPUT,${IntegrationID},1,${DimmerLevel}
    //
    char *endPtr;
    // Find keypad that matches the ${IntegrationID}.
    int id = (int)strtol(line.c_str() + 8, &endPtr, 10);

    // While we are gradually fading DMX dimmers in response to the user
    // holding down the dimmer button, we don't respond to Lutron updating
    // this fixture. That would just result in needless flicker.
    suppressed = suppressDummyDimmer_.find(id) != suppressDummyDimmer_.end();
    if (!suppressed) {
      const auto& out = outputs_.find(id);
      if (out != outputs_.end() &&
          *endPtr++ == ',' && *endPtr++ == '1' && *endPtr++ == ',') {
        const auto newLevel = strToLevel(endPtr);
        const auto it = releaseDummyDimmer_.find(id);
        if (it != releaseDummyDimmer_.end() && Util::millis() < it->second &&
            out->second.level != newLevel) {
          // Looks as if Lutron wants to override the value that we just set
          // for the dimmer moments earlier. Fix that now.
          command(fmt::format("#OUTPUT,{},1,{}.{:02}", id,
                              out->second.level/100, out->second.level%100));
          suppressed = true;
        } else {
          // Update our internal state.
          out->second.level = newLevel;
          context = out->second.name;

          // Notify listeners, if any.
          const auto it = outputMonitor_.find(id);
          if (it != outputMonitor_.end()) {
            it->second(newLevel);
          }

          // Check if there is any aliased output. This allows us to take over
          // the implementation of an output that is *also* natively handled by
          // the Lutron controller.
          const std::string& alias = fmt::format("{}{}", ALIAS, id);
          const std::string& dmxalias = fmt::format("{}{}", DMXALIAS, id);
          for (auto& [name, level, cb] : namedOutput_) {
            if (name == alias || name == dmxalias) {
              if (level != out->second.level && cb) {
                event_.runLater([=]() { cb(out->second.level, true); });
              }
              level = out->second.level;
            }
          }
          broadcastDimmerChanges(id);
        }
      }
    }
  } else if (Util::starts_with(line, "~SYSTEM,1,")) {
    // When the controller tells us the system time, compare it to our own
    // understanding of time. If there is a significant difference, we can
    // adjust the time as needed.
    unsigned char h, m, s;
    if (sscanf(line.c_str() + 10, "%2hhu:%2hhu:%2hhu%*c", &h, &m, &s) != 3) {
      DBG("Cannot parse time string");
    } else {
      time_t ti = time(NULL);
      struct tm tm;
      localtime_r(&ti, &tm);
      unsigned d = ((s + 60*(m + 60*(unsigned)h)) + 86400 -
                    (tm.tm_sec + 60*(tm.tm_min + 60*tm.tm_hour))) % 86400;
      if (d >= 43200) d = 86400 - d;
      if (d > 3) {
        DBG("Time has drifted " << d << " seconds");
        command(fmt::format("#SYSTEM,1,{:02}:{:02}:{:02}",
                            tm.tm_hour, tm.tm_min, tm.tm_sec));
      }
    }
  }

  // A short while after the last update, we recompute all LEDs.
  if (recompute_ || !lutron_.commandPending()) {
    event_.removeTimeout(recompute_);
    recompute_ = event_.addTimeout(200, [this]() {
      recompute_ = nullptr;
      recomputeLEDs();
    });
  }
  if (input_ && !suppressed) {
    input_(line, Util::trim(context), true);
  }
}

void RadioRA2::init(std::function<void (void)> cb) {
  DBG("Connection opened");
  // Sanity check. If we never actually succeeded in opening the connection,
  // close it down properly (if not already done) and notify our caller.
  if (!lutron_.isConnected()) {
    lutron_.closeSock();
    if (cb) {
      cb();
    }
    return;
  }

  // Initialize the connection. Most notably, that means turning on all
  // the notifications that we are interested in.
  static const MonitorType events[] = {
    MONITOR_BUTTON, MONITOR_LED, MONITOR_OCCUPANCY, MONITOR_PHOTOSENSOR,
    MONITOR_OCCUPANCYGRP };
  for (const auto& ev : events) {
    command(fmt::format("#MONITORING,{},1", ev));
  }

  if (!devices_.size() && !outputs_.size()) {
    // Getting the schema takes a really long time. We should only ever do
    // so once and then cache the result even if we needed to reset the
    // connection.
    pugi::xml_document xml;
    if (xml.load_file(".lutron.xml")) {
      extractSchemaInfo(xml);
    }
  } else {
    // If we already had a cached copy of the schema, all we have to do is
    // sync our internal state with the state of the Lutron system. We were
    // probably disconnected because of networking problems, and might have
    // missed some status updates.
    refreshCurrentState(cb);
    cb = nullptr;
  }

  // Asynchronously re-read the schema information from the Lutron device and
  // make sure it still matches. Otherwise, we restart and reset our entire
  // state.
  // This code also runs, if we never had any cached data in the first place.
  struct sockaddr_storage addr;
  socklen_t addrLen = sizeof(addr);
  if (lutron_.getConnectedAddr((sockaddr&)addr, addrLen) &&
      (addr.ss_family == AF_INET || addr.ss_family == AF_INET6)) {
    DBG("Retrieving XML data from Lutron device");
    ((struct sockaddr_in&)addr).sin_port = htons(80);
    getSchema((const sockaddr&)addr, addrLen, [=, this]() {
      refreshCurrentState([=, this]() {
        // If this is the first time that we have seen any automation schema,
        // there will be init_ handlers that need to be notified. Remove them
        // afterwards.
        const auto init = std::move(init_);
        for (const auto& o : init) {
          if (o) {
            event_.runLater(o);
          }
        }
        if (cb) {
          cb();
        }
        initialized_ = true;
      }); });
  } else {
    DBG("Failed to get XML schema; device not found");
    lutron_.closeSock();
    if (cb) {
      cb();
    }
  }
}

void RadioRA2::closed() {
  DBG("Connection closed");
  if (schemaSock_ >= 0) {
    // If something went wrong while we were reading the schema, delete all
    // partial state and start over again next time. The health check will
    // open up the connection soon enough.
    //
    // In general though, we only ever read the schema once at program
    // start up. So, this code path can only be traversed if we immediately
    // encountered a networking issue when the program first began running.
    // It will resolve itself, as soon as the underlying problem has been
    // taken care of.
    DBG("Incomplete schema");
    close(schemaSock_);
    event_.removePollFd(schemaSock_);
    schemaSock_ = -1;
    devices_.clear();
    outputs_.clear();
  }
}

void RadioRA2::getSchema(const sockaddr& addr, socklen_t len,
                         std::function<void ()> cb) {
  if (cb && (devices_.size() || outputs_.size()) && schemaInvalid_) {
    // If we already have cached data, continue initialization speculatively
    // and restart the daemon if necessary.
    event_.runLater(cb);
    cb = nullptr;
  }
  // Read the schema for our automation system from the Lutron gateway
  // device. This happens asynchronously as it is a very slow operation.
  // The device sends about 128kB of data in 1kB chunks with noticeable
  // delays between each of them.
  schemaSock_ = socket(addr.sa_family,
                       SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC,
                       IPPROTO_TCP);
  if (schemaSock_ < 0) {
  err:
    lutron_.closeSock();
    if (cb) {
      cb();
    }
    return;
  }
  // We let the event loop drive the operation. First, we wait for the
  // connection to be established.
  const auto readSchema = [cb, this](pollfd *) {
    event_.removePollFd(schemaSock_);
    socklen_t addrlen = 0;
    if (getpeername(schemaSock_, (struct sockaddr *)"", &addrlen) < 0) {
      DBG("Failed to connect");
    err:
      close(schemaSock_);
      schemaSock_ = -1;
      lutron_.closeSock();
      if (cb) {
        cb();
      }
      return true;
    }
    // Once the socket is connected and ready to accept data, we send a
    // basic HTTP request for the schema.
    const char GET[] = "GET /DbXmlInfo.xml HTTP/1.0\r\n\r\n";
    if (write(schemaSock_, GET, sizeof(GET)-1) != sizeof(GET)-1) {
      DBG("Unexpected failure to write to socket");
      goto err;
    }
    // All data is read asynchronously from the event loop.
    event_.addPollFd(schemaSock_, POLLIN,
      [this, cb, schema = std::string()](auto) mutable {
      char buf[1100];
      for (;;) {
        const auto rc = read(schemaSock_, buf, sizeof(buf));
        if (rc > 0) {
          // Append data to buffer and try reading more until we encounter
          // a short read. Then return to the event loop.
          lutron_.initStillWorking();
          schema += std::string(buf, rc);
          if (rc == sizeof(buf)) {
            continue;
          }
          return true;
        } else if (rc == 0) {
          // Encountered end of stream.
          const auto& xmlSource = schema.find("\r\n<?xml ");
          if (xmlSource == std::string::npos) {
            schema = "";
          } else {
            schema = schema.substr(xmlSource + 2);
          }
          DBG("Read " << schema.size() << " bytes of schema information");
          close(schemaSock_);
          schemaSock_ = -1;
          pugi::xml_document xml;
          if (!schema.size() ||
              !xml.load_buffer(schema.c_str(), schema.size())) {
            // If anything went wrong, we close the other (!) socket to
            // the Lutron device. This resets everything and there will
            // be a retry in a short while.
            lutron_.closeSock();
          } else {
            // The XML data is extremely unwieldy. It contains more information
            // than we really need. And it also organizes the data in a way
            // that makes it hard to  manipulate. Extract only what we need
            // and populate our internal data structures.
            if (extractSchemaInfo(xml)) {
              DBG("Cached schema is invalid; updating cache with new data");
              // While we could save the raw data returned from the device,
              // we instead pretty-print it. That can help when debugging a
              // site's configuration.
              if (!xml.save_file(".lutron.xml", "  ")) {
                if (!cb && schemaInvalid_) {
                  schemaInvalid_();
                }
              }
            } else {
              DBG("Cached data is unchanged");
            }
          }
          if (cb) {
            cb();
          }
          return false;
        } else if (errno == EINPROGRESS || errno == EWOULDBLOCK) {
          // Return to event loop and keep reading when more data arrives.
          return true;
        } else {
          // Unexpected I/O error. Discard all data and reset connection.
          DBG("Failed to read schema");
          close(schemaSock_);
          schemaSock_ = -1;
          lutron_.closeSock();
          if (cb) {
            cb();
          }
          return false;
        }
      }
    });
    return true;
  };
  if (connect(schemaSock_, &addr, len) >= 0) {
    // connect() usually returns asynchronously, as we configured the socket
    // to be non-blocking. But if it returns synchronously, that's OK too.
    readSchema(nullptr);
  } else {
    if (errno == EINPROGRESS || errno == EWOULDBLOCK) {
      event_.addPollFd(schemaSock_, POLLOUT, readSchema);
    } else {
      goto err;
    }
  }
}

int RadioRA2::strToLevel(const char *ptr) {
  // The dimmer level is usually a number between 0 and 100 and has a
  // two digit precision. We store this information in a fixed point
  // integer number to avoid rounding issues.
  int l = 100*(int)strtol(ptr, (char **)&ptr, 10);
  if (*ptr == '.' && ptr[1] >= '0' && ptr[1] <= '9') {
    l += 10*(ptr[1] - '0');
    if (ptr[2] >= '0' && ptr[2] <= '9') {
      l += ptr[2] - '0';
    }
  }
  return std::max(0, std::min(10000, l));
}

bool RadioRA2::extractSchemaInfo(pugi::xml_document& xml) {
  // There is a lot more data in the XML file than what we actually need.
  // Only extract the important information and store it an a much more
  // manageable internal data structure.
  // Also, we haven't seen all the possible combination of parameters. Some
  // of the more advanced features are not in use in our system. We support
  // keypads and light fixtures. Anything else would need additional code
  // to support it. Refer to
  // https://www.lutron.com/TechnicalDocumentLibrary/040249.pdf for details.

  // Convert string representation of device types to internal enum type.
  static const auto deviceType = [](const std::string& type) {
    static struct { DeviceType t; const char *s; } table[] = {
      { DEV_PICO_KEYPAD, "PICO_KEYPAD" },
      { DEV_SEETOUCH_KEYPAD, "SEETOUCH_KEYPAD" },
      { DEV_HYBRID_SEETOUCH_KEYPAD, "HYBRID_SEETOUCH_KEYPAD" },
      { DEV_MOTION_SENSOR, "MOTION_SENSOR" },
      { DEV_MAIN_REPEATER, "MAIN_REPEATER" } };
    for (const auto& t : table) {
      if (type == t.s) return t.t;
    }
    return DEV_UNKNOWN;
  };

  // Convert string representation of button types to internal enum.
  static const auto buttonType = [](const std::string& type) {
    static struct { ButtonType t; const char *s; } table[] = {
      { BUTTON_TOGGLE, "Toggle" },
      { BUTTON_ADVANCED_TOGGLE, "AdvancedToggle" },
      { BUTTON_SINGLE_ACTION, "SingleAction" },
      // We treat the raise/lower buttons on the Pico as if they were
      // regular scene buttons. This might need revisiting.
      { BUTTON_SINGLE_ACTION, "SingleSceneRaiseLower" },
      { BUTTON_SINGLE_ACTION, "SingleSceneRaiseLower" },
      { BUTTON_LOWER, "MasterRaiseLower" } };
    for (const auto& t : table) {
      if (type == t.s) return t.t;
    }
    return BUTTON_UNKNOWN;
  };

  // Iterate over all devices (i.e. keypads, repeaters, motion sensors, ...)
  std::map<int, Device> devices;
  const auto& devs = xml.select_nodes("//Device");
  for (const auto& device : devs) {
    Device dev{
      device.node().attribute("IntegrationID").as_int(-1),
      device.node().attribute("Name").value(),
      deviceType(device.node().attribute("DeviceType").value())};

    // Iterate over all buttons that are part of this device/keypad.
    const auto& components = device.node().select_nodes(".//Button");
    for (const auto& component : components) {
      pugi::xpath_variable_set vars;
      vars.add("model", pugi::xpath_type_string);
      vars.set("model",
               component.node().attribute("ProgrammingModelID").value());

      // Buttons can have an LED associated with it. This information is stored
      // in a separate XML section.
      const auto& led = component.node().select_node(
        "//LED[@ProgrammingModelID=string($model)]", &vars);
      auto type =
        buttonType(component.node().attribute("ButtonType").value());
      // While there is both a lower and a raise button, they have the same
      // button type. It's easier to disambiguate this information while
      // parsing the XML data and assign two distinct button types.
      if (type == BUTTON_LOWER &&
          component.node().attribute("Direction").value() ==
          std::string("Raise")) {
        type = BUTTON_RAISE;
      }
      Component comp{
        component.parent().attribute("ComponentNumber").as_int(-1),
        led.parent().attribute("ComponentNumber").as_int(-1),
        component.node().attribute("Engraving").value(),
        (LedLogic)component.node().attribute("LedLogic").as_int(0),
        type};

      // Iterate over all actions that have been assigned to the buttons.
      // Generally, these are just light levels for different outputs.
      const auto& assignments =
        component.node().select_nodes(".//PresetAssignment[@AssignmentType=\"2\"]");
      for (const auto& assign : assignments) {
        // Convert dimmer level to our internal representation.
        Assignment as{atoi(assign.node().child_value("IntegrationID")),
                      strToLevel(assign.node().child_value("Level"))};
        comp.assignments.push_back(as);
      }
      dev.components[comp.id] = comp;
    }
    devices[dev.id] = dev;
  }

  // Iterate over all outputs (i.e. light fixtures)
  std::map<int, Output> outputs;
  const auto& outs = xml.select_nodes("//Output");
  for (const auto& output : outs) {
    Output out(output.node().attribute("IntegrationID").as_int(-1),
               output.node().attribute("Name").value());
    outputs[out.id] = out;
  }

  if (devices_ == devices && outputs_ == outputs) {
    return false;
  } else {
    devices_ = std::move(devices);
    outputs_ = std::move(outputs);
    return true;
  }
}

void RadioRA2::refreshCurrentState(std::function<void ()> cb) {
  // This part of the initialization procedure can take a while. We
  // therefore have to submit a sequence of "Lutron::command()"s
  // and then invoke "Lutron::initStillWorking()" from the callbacks of that
  // function.
  for (const auto& out : outputs_) {
    // Iterate over all light fixtures and query their state.
    command(fmt::format("?OUTPUT,{},1", out.second.id),
            [this](auto) { lutron_.initStillWorking(); });
  }

  // Invoke our callback, when all pending commands have completed. This is
  // much easier than explicitly keeping track of a chain of callbacks.
  // We don't actually wait for the LED status to be updated. Overall,
  // intialization is quite slow because of all the communication involved.
  // Speculatively initialize things as fast as we can, and then fix things
  // up asynchronously as needed.
  command("", [cb, this](auto) {
    if (cb) {
      cb();
    }
    event_.addTimeout(2000, [this]() {
      for (auto& dev : devices_) {
        for (auto& comp : dev.second.components) {
          if (comp.second.led < 0) {
            continue;
          }
          // LED state isn't always perfectly tracked by the Lutron controller.
          // Besides 0 and 1, it can also return 255. In that case, we assume
          // that the LED is unchanged or turned off.
          if (ledState_ &&
              (dev.second.type == DEV_SEETOUCH_KEYPAD ||
               dev.second.type == DEV_HYBRID_SEETOUCH_KEYPAD)) {
            const int level = getLevelForButton(comp.second.assignments);
            ledState_(dev.second.id, comp.second.id, false, level);
          }
          comp.second.ledState = 0;
          command(fmt::format("?DEVICE,{},{},{}",
                              dev.second.id,
                              comp.second.led,
                              ACTION_LEDSTATE),
                  [this](auto) { lutron_.initStillWorking(); });
        }
      }
    });
  });
}

int RadioRA2::getCurrentLevel(int id) {
  // Return the current light level for one of our outputs. Positive "id"
  // numbers refer to the Lutron integration id. Negative numbers are our
  // own virtual outputs which can map to DMX dimmers or GPIO output devices.
  if (id < 0 && -id <= (int)namedOutput_.size()) {
    return std::min(std::max(namedOutput_[-id-1].level, 0), 10000);
  } else {
    const auto& out = outputs_.find(id);
    if (out != outputs_.end()) {
      return out->second.level;
    } else {
      DBG("Can't find output " << id);
    }
  }
  return -1;
}

int RadioRA2::getLevelForButton(const std::vector<Assignment>& assignments) {
  // The web UI would like to show a visual representation of the dimmer
  // state while it is being adjusted. This is complicated by the fact that
  // it doesn't actually show information on fixtures at any point. Instead,
  // it only represents keypads. Keypads can have fixtures assigned to their
  // buttons though. Make a reasonable effort to compute the current
  // dimmer state that matches an assignment.
  int level = 0;
  for (const auto& as : assignments) {
    const int assignedLevel = as.level;
    const int currentLevel = getCurrentLevel(as.id);
    if (!assignedLevel) {
      continue;
    }
    level = std::max(level, std::min(10000, currentLevel));
  }
  return level;
}

void RadioRA2::broadcastDimmerChanges(int id) {
  // The web UI wants to show dimmer levels as they are changing.
  // The same fixture can be assigned to multiple buttons. Find all
  // buttons that match.
  if (ledState_) {
    for (const auto& [ _, dev ] : devices_) {
      for (const auto& [ _, btn ] : dev.components) {
        for (const auto& as : btn.assignments) {
          if (as.id == id) {
            const int level = getLevelForButton(btn.assignments);
            ledState_(dev.id, btn.id, (int)btn.ledState, level);
            goto nextButton;
          }
        }
      nextButton:;
      }
    }
  }
}

void RadioRA2::recomputeLEDs() {
  // Iterate over all keypads and LEDs and compute the expected LED state.
  for (const auto& [_, device] : devices_) {
    for (const auto& [_, component] : device.components) {
      // We only support LED_MONITOR and LED_SCENE at this time.
      if (component.led >= 0 &&
          (component.logic == LED_MONITOR ||
           component.logic == LED_SCENE)) {
        bool ledState = component.logic == LED_SCENE;
        bool empty = true;
        for (const auto& as : component.assignments) {
          int level = getCurrentLevel(as.id);
          if (level < 0) {
            continue;
          }
          empty = false;
          if (component.logic == LED_MONITOR) {
            if (level > 0) {
              // LED is on when at least one device is at any level
              ledState = true;
            }
          } else if (level != as.level) {
            // LED is on when all devices are at the exact programmed level
            ledState = false;
          }
        }
        // If there actually aren't any fixtures associated with this LED,
        // it is always off.
        ledState &= !empty;

        // If there is a mismatch in LED status, fix that now.
        if (ledState != component.ledState) {
          DBG("LED \"" << component.name << "\" (" <<
              component.id << ") on \"" << device.name <<
              "\" should be " << (ledState ? "on" : "off"));
          if (ledState_ &&
              (device.type == DEV_SEETOUCH_KEYPAD ||
               device.type == DEV_HYBRID_SEETOUCH_KEYPAD)) {
            ledState_(device.id, component.id, ledState,
                      getLevelForButton(component.assignments));
          }
          command(fmt::format("#DEVICE,{},{},9,{}",
            device.id, component.led, ledState ? 1 : 0));
        }
      }
    }
  }
}

void RadioRA2::addButtonListener(int kp, int bt,
                           std::function<void (int, int, bool, bool, int)> cb) {
  const auto dev = devices_.find(kp);
  if (dev != devices_.end()) {
    const auto keypad = dev->second.components.find(bt);
    if (keypad != dev->second.components.end()) {
      keypad->second.listeners.push_back(cb);
    }
  }
}

void RadioRA2::monitorOutput(int id, std::function<void (int level)> cb) {
  outputMonitor_[id] = cb;
}

int RadioRA2::addOutput(const std::string name,
                        std::function<void (int, bool)> cb) {
  // Returns a previously registered virtual output device that is identified
  // by its unique name. If no such device exists, register a new one and
  // assign it an id number. This number is always negative to distinguish it
  // from Lutron integration ids.
  int id;
  const auto& it = std::find_if(namedOutput_.begin(), namedOutput_.end(),
                                [&](const auto &n) { return n.name == name; });
  if (it == namedOutput_.end()) {
    namedOutput_.push_back(NamedOutput{name, 0, cb});
    id = -namedOutput_.size();
  } else {
    id = -(it - namedOutput_.begin() + 1);
  }
  return id;
}

void RadioRA2::addToButton(int kp, int bt, int id, int level, bool makeToggle) {
  // The Lutron device obviously only knows about Lutron output devices. But
  // we would like to add virtual output devices that can control DMX fixtures
  // or GPIO output pins. After registering a new device with "addOutput()",
  // "addToButton()" allows for adding these devices as an assignment to an
  // existing Lutron keypad button.
  // Optionally, it is possible to override the Lutron button type and force the
  // button to behave as a toggle switch. This is helpful for PicoRemote buttons
  // which usually can only activate predefined scenes.
  if (devices_.find(kp) == devices_.end()) {
    DBG("Cannot find keypad with id " << kp);
    return;
  } else if (devices_[kp].components.find(bt) == devices_[kp].components.end()){
    DBG("Cannot find component " << kp << "/" << bt);
    return;
  } else if (std::find_if(devices_[kp].components[bt].assignments.begin(),
                          devices_[kp].components[bt].assignments.end(),
                          [&](auto a) { return a.id == id; }) !=
             devices_[kp].components[bt].assignments.end()) {
    DBG("Duplicate assignment for " << kp << "/" << bt << ", fixture " << id);
    return;
  }
  if (makeToggle) {
    if (devices_[kp].components[bt].type != BUTTON_TOGGLE &&
        devices_[kp].components[bt].assignments.size()) {
      DBG("Contradictory constraints; cannot override button type");
    } else {
      devices_[kp].components[bt].type = BUTTON_TOGGLE;
    }
  }
  devices_[kp].components[bt].assignments.push_back(
    Assignment{id, level == -1 ? -1 : level*100});
}

void RadioRA2::toggleOutput(int out) {
//DBG("RadioRA2::toggleOutput(" << out << ")");
  auto output = outputs_.find(out);
  if (output != outputs_.end()) {
    auto& level = output->second.level;
    level = level ? 0 : 10000;
    command(fmt::format("#OUTPUT,{},1,{}.{:02}",
                        out, level/100, level%100));
  }
}

void RadioRA2::command(const std::string& cmd,
                       std::function<void (const std::string& res)> cb,
                       std::function<void (void)> err) {
  if (Util::starts_with(cmd, "#DEVICE,")) {
    char *endPtr;
    const auto& dev = devices_.find((int)strtol(cmd.c_str() + 8, &endPtr, 10));
    if (dev != devices_.end() && *endPtr++ == ',') {
      int l = (int)strtol(endPtr, &endPtr, 10);
      auto& keypad = dev->second;
      const auto comp = keypad.components.find(l);
      if (comp != keypad.components.end() && !keypad.supportsReleaseEvent &&
          comp->second.type != BUTTON_LOWER &&
          comp->second.type != BUTTON_RAISE &&
          !strcmp(endPtr, ",4")) {
        // There are a couple of situations where we generate synthetic
        // button events that will then be sent to the Lutron main repeater.
        // These are events of the form "#DEVICE,<keypad>,<button>,{3,4}".
        // This can be confusing, if the actual device isn't capable of
        // producing ACTION_RELEASE events, as some keypads are wont to do.
        // When dealing with that type of device, we suppress the button up
        // event altogether, so that our code experiences the same type of
        // events no matter whether they come from the physical device or
        // were produced on the fly.
        return;
      }
    }
  }
  lutron_.command(cmd, cb, err);
}

std::string RadioRA2::getKeypads(const std::vector<int>& order) {
  // Returns a simplified snapshot of our internal state in JSON format.
  // All strings need to be escaped first. We also remove inlined configuration
  // data that follows a ":" colon.
  const auto esc = [](const std::string &s) {
    std::string str(s);
    const auto it = str.find(':');
    if (it != std::string::npos) {
      str.erase(it);
    }
    str = Util::trim(str);
    std::ostringstream o;
    for (auto c = str.cbegin(); c != str.cend(); c++) {
      switch (*c) {
      case '\x00' ... '\x09':
      case '\x0b' ... '\x1f':
        o << fmt::format("\\u{:04x}", (unsigned)(unsigned char)*c);
        break;
      case '\x0a': o << "\\n"; break;
      case '\x22': o << "\\\""; break;
      case '\x5c': o << "\\\\"; break;
      default: o << *c;
      }
    }
    return o.str();
  };
  std::ostringstream str;
  str << "[";
  // Iterate over all devices, but only return information for actual keypads.
  // Most notably, this skips over the virtual buttons associated with the
  // Lutron controller itself.
  // If the caller requested a particular order of keypads, enforce that now.
  // Add any missing keypads that weren't listed already.
  // Omit any devices that have a negative id in the "order" vector.
  std::vector<int> ids;
  std::copy_if(order.begin(), order.end(), back_inserter(ids),
    [this](const int i) { return devices_.find(i) != devices_.end(); });
  for (const auto& [ id, dev ] : devices_) {
    if (dev.type != DEV_PICO_KEYPAD &&
        dev.type != DEV_SEETOUCH_KEYPAD &&
        dev.type != DEV_HYBRID_SEETOUCH_KEYPAD) {
      continue;
    }
    if (std::find(ids.begin(), ids.end(), id) == ids.end() &&
        std::find(order.begin(), order.end(), -id) == order.end()) {
      ids.push_back(id);
    }
  }
  bool firstKeypad = true;
  for (const int id : ids) {
    const auto& dev = devices_[id];
    // Keeping track of whether to include a trailing "," comma is tedious.
    if (firstKeypad) {
      firstKeypad = false;
    } else {
      str << ",";
    }
    // Output the name of the keypad, the LEDs and the buttons.
    str << fmt::format("{{\"id\":{},\"label\":\"{}\",\"leds\":{{",
                       dev.id, esc(dev.name));
    bool firstLed = true;
    for (const auto& [ _, button ] : dev.components) {
      if (button.led < 0) {
        continue;
      }
      if (firstLed) {
        firstLed = false;
      } else {
        str << ",";
      }
      str << fmt::format("\"{}\":{}", button.id, (int)button.ledState);
    }
    str << "},\"buttons\":{";
    bool firstButton = true;
    for (const auto& [ _, button ] : dev.components) {
      if (firstButton) {
        firstButton = false;
      } else {
        str << ",";
      }
      str << fmt::format("\"{}\":", button.id);
      // Dimmer buttons are encoded as booleans to make them easy to
      // identify. Other buttons are stored with their label.
      if (button.type == BUTTON_LOWER || button.type == BUTTON_RAISE) {
        str << (button.type != BUTTON_LOWER ? "true" : "false");
      } else {
        str << fmt::format("\"{}\"", esc(button.name));
      }
    }
    str << "},\"dimmers\":{";

    bool firstDimmer = true;
    for (const auto& [ _, button ] : dev.components) {
      if (button.led < 0) {
        continue;
      }
      if (firstDimmer) {
        firstDimmer = false;
      } else {
        str << ",";
      }
      const auto dimmer = getLevelForButton(button.assignments);
      str << fmt::format("\"{}\":{}.{:02}", button.id, dimmer/100, dimmer%100);
    }
    str << "}}";
  }
  str << "]";
  return str.str();
}

void RadioRA2::updateEnvironment() {
  std::ostringstream o;
  int i = 0;
  for (auto iter = outputs_.begin(); iter != outputs_.end(); i++) {
    while (i < iter->first) {
      o << "'' ";
      i++;
    }
    o << iter->second.level;
    if (iter++ == outputs_.end()) {
      break;
    } else {
      o << " ";
    }
  }
  setenv("OUTPUTS", o.str().c_str(), 1);
}

void RadioRA2::suppressLutronDimmer(int id, bool mode) {
  // Lutron eventually responds to dimmer button presses by sending a new
  // value for the dimmer, even if this is a dummy device for our DMX
  // fixture and dimming is fully managed by us. We carefully manage when
  // we expect these events to show up and suppress them. But if there is
  // a rapid sequence of events, Lutron can fall behind and send it's
  // update delayed. When that happens, we need to make sure we ignore
  // for a little longer.
  if (mode) {
    suppressDummyDimmer_.insert(id);
  } else if (suppressDummyDimmer_.find(id) != suppressDummyDimmer_.end()) {
    suppressDummyDimmer_.erase(id);
    releaseDummyDimmer_[id] = Util::millis() + 200;
  }
}

void RadioRA2::setDMXorLutron(int id, int level, bool fade, bool suppress,
                              bool noUpdate) {
  // Things get a little tricky when adjusting ouput levels in response
  // to the user pushing the dimmer buttons, as Lutron will tell us it wants
  // to set the wrong value. After all, the physical keypad still
  // controls the dummy device, and Lutron sets its final value when
  // the user releases the dimmer button. This would all be fine, if it also
  // smoothly progressed through brightness values and reported those
  // to us. But the Lutron controller does not do this for DMX loads
  // when using the integration API; requiring us to do all the heavy
  // lifting ourselves.
  // This also means, we need to suppress the bogus updates that Lutron
  // reports after the button is released, as it almost certainly is
  // going to be in disagreement with the value that we determined.
  // We have to make sure not to invoke the "input_" callback nor
  // update our own cached value in "outputs_[id].level".
  // Instead, we queue our own command, which informs Lutron of the
  // level that we would like it to have. Unfortunately, the asynchronous
  // execution means we might or might not also filter out the return status
  // from out own command. So, we have to additionally invoke the "input_"
  // callback instead of relying on the usual callbacks to do so for us,
  // when they saw that Lutron accepted the value.
  if (id < 0) {
    if (id >= 0 || -id > (int)namedOutput_.size()) {
      DBG("Invalid id " << id);
      return;
    }
    auto& out = namedOutput_[-id-1];
    if (out.level != level) {
      out.level = std::min(10000, std::max(0, level));
      out.cb(level, fade);
    }
    broadcastDimmerChanges(id);
  } else {
    auto it = outputs_.find(id);
    if (it == outputs_.end()) {
      DBG("Invalid id " << id);
      return;
    }
    auto& out = it->second;
    if (suppress) {
      suppressLutronDimmer(id, true);
    }
    if (!noUpdate) {
      // NoUpdate must not be set on the very final call, as that call both
      // flushes our cached state to the Lutron system and removes the
      // suppression.
      command(fmt::format("#OUTPUT,{},1,{}.{:02}",
                          id, level/100, level%100),
              suppress ? [this, id](auto) { suppressLutronDimmer(id, false); }
                       : (std::function<void (const std::string&)>)nullptr);
    }
    if (out.level != level) {
      out.level = level;
      broadcastDimmerChanges(id);
      if (input_) {
        input_(fmt::format("~OUTPUT,{},1,{}.{:02}",
                           id, level/100, level%100),
               Util::trim(out.name), fade);
      }
    }
  }
}

void RadioRA2::buttonPressed(Device& keypad, Component& button,
                             bool isReleased) {
  const auto now = Util::millis();

  // Invoke any listeners that are interested in this button (if any).
  if (!isReleased) {
    for (const auto& listener : button.listeners) {
      listener(keypad.id, button.id, false, false, 0);
    }
  } else {
    if (button.type == BUTTON_TOGGLE ||
        button.type == BUTTON_ADVANCED_TOGGLE ||
        button.type == BUTTON_SINGLE_ACTION) {
      // Some keypads report ACTION_PRESS/ACTION_RELEASE, whereas others
      // only report ACTION_PRESS
      keypad.supportsReleaseEvent = true;
    }
  }

  // Detect double and long presses, where possible, and provide this
  // information to any listeners. This is currently only implement for the
  // main buttons on keypads (not for dimmer buttons) and for Pico buttons.
  // It is intended to be used by external helper scripts that trigger more
  // complex functions.
  if (button.type == BUTTON_TOGGLE ||
      button.type == BUTTON_ADVANCED_TOGGLE ||
      button.type == BUTTON_SINGLE_ACTION) {
    // Depending on the model of the SeeTouch keypad, there might or might
    // not be "release" events for regular keypad buttons. There should always
    // be "release" event for the dimmer buttons on the keypad.
    // On the other hand, for Pico remotes, it is possible to track long
    // button presses.
    if (keypad.dimDirection || keypad.lastButton != button.id ||
        keypad.firstTap == 0) {
      keypad.lastButton = button.id;
      keypad.dimDirection = 0;
      keypad.startOfDim = now;
      keypad.firstTap = now;
      keypad.numTaps = 1;
      keypad.on = false;
    } else {
      if (!isReleased) {
        keypad.numTaps++;
      }
      keypad.startOfDim = now;
    }
    if (isReleased) {
      keypad.released = now;
    } else {
      keypad.released = 0;
    }

    event_.addTimeout(
      // If this event included a release event, we can make a more reasonable
      // guess for how long to wait.
      isReleased
        ? std::max(300u, std::min(
          keypad.type == DEV_PICO_KEYPAD ? DOUBLETAP : LONGDOUBLETAP,
          (now - (keypad.startOfDim == now
                  ? keypad.firstTap : keypad.startOfDim))*3/2))
        : keypad.type == DEV_PICO_KEYPAD
        ? LONGPICO
        : LONGDOUBLETAP,
      [this, firstTap = keypad.firstTap, startOfDim = keypad.startOfDim,
       numTaps = keypad.numTaps, rel = keypad.released, &keypad, &button]() {
        if (firstTap != keypad.firstTap || numTaps != keypad.numTaps ||
            rel != keypad.released) {
          // There have been more taps since. Ignore this callback.
          return;
        }
        // Sufficient time has past since the last button event. We
        // are certain we now have this type of button press identified.
        bool isLong = keypad.supportsReleaseEvent && !rel;
        keypad.numTaps = 0;
        keypad.firstTap = 0;
        for (const auto& listener : button.listeners) {
          listener(keypad.id, button.id, keypad.on, isLong, numTaps);
        }
      });
  }

  // Replicate the same logic that Lutron does for button presses, but
  // apply it to non-Lutron virtual outputs (e.g. DMX fixtures).
  switch (button.type) {
  // Toggle control / Room monitoring
  case BUTTON_TOGGLE:
  case BUTTON_ADVANCED_TOGGLE: {
    // Toggle switches between on and off for all devices in this assignments.
    keypad.on = false;
    for (const auto& as : button.assignments) {
      if (as.level == -1) {
        // Relays don't have an on/off state; they always toggle when activated.
        continue;
      }
      keypad.on |= getCurrentLevel(as.id) > 0;
    }
    // Aliased outputs that refer to DMX fixtures have to be set by us, each
    // time a button is pressed.
    for (const auto& as : button.assignments) {
      if (as.id < 0) {
        setDMXorLutron(as.id, as.level == -1 ? -1 : keypad.on ? 0 : as.level,
                       true);
      }
    }
    keypad.on = !keypad.on;
    break; }

  // Single/Multi-room scene
  case BUTTON_SINGLE_ACTION:
    // Aliased outputs that refer to DMX fixtures have to be set by us, each
    // time a button is pressed.
    for (const auto& as : button.assignments) {
      if (as.id < 0) {
        setDMXorLutron(as.id, as.level, true);
      }
    }
    break;

  // Dimmer control buttons
  case BUTTON_LOWER:
  case BUTTON_RAISE: {
    // Button pressed:
    if (keypad.lastButton < 0 ||
        keypad.components.find(keypad.lastButton) == keypad.components.end()){
      DBG("No last button known for this keypad");
      return;
    }

    if (isReleased) {
      // If using dummy output devicess, both Lutron and us will try to adjust
      // the dimmer. Avoid flashing, by forcing the final value to the one that
      // we picked.
      std::map<int, int> targetLevels;
      for (const auto& as : keypad.components[keypad.lastButton].assignments) {
        // The final dimmer level might need adjustment. If the user only
        // pressed the button for a short amount of time, jump a full increment.
        int level;
        if (as.id < 0) {
          // DMX dimmer configured in "site.json".
          if (-as.id <= (int)namedOutput_.size()) {
            if (as.level == -1) {
              // This is a relay. Don't even try to dim it.
              continue;
            }
            level = namedOutput_[-as.id-1].level;
          }
        } else {
          auto it = outputs_.find(as.id);
          if (it != outputs_.end() &&
              it->second.name.find(':') != std::string::npos) {
            // DMX dimmer defined inline in the name of a Lutron "dummy" device.
            level = it->second.level;
          } else {
            // This dimmer is controlled by Lutron, leave it alone.
            continue;
          }
        }

        // If we no longer remember the starting brightness, that means that
        // dimSmooth() ran into the maximum value. No need to do any additional
        // step to a full increment. We're already there.
        auto it = keypad.startingLevels.find(as.id);
        if (it != keypad.startingLevels.end()) {
          // Snap to a discrete dim level, either up or down.
          if (keypad.dimDirection < 0) {
            level = std::min(level,
                 (int)(((DIMLEVELS*it->second+5000)/10000-1)*10000/DIMLEVELS));
          } else if (keypad.dimDirection > 0) {
            level = std::max(level,
                 (int)(((DIMLEVELS*it->second+5000)/10000+1)*10000/DIMLEVELS));
          }
        }
        level = std::min(10000, std::max(0, level));
        targetLevels[as.id] = level;
      }

      // Handle double taps by jumping the dimmer to the most extreme values
      // (i.e. fully on or off). But ignore triple taps or more. That would
      // happen if the user is rapidly pushing the button to step through
      // multiple discrete intervals because they don't like smooth adjustments.
      // In those situations we don't want to confuse them by jumping all the
      // way.
      if (keypad.dimDirection != 0 && keypad.numTaps == 2 &&
          now - keypad.firstTap < DOUBLETAP) {
        const int jumpedLevel = keypad.dimDirection > 0 ? 10000 : 0;
        // Base the estimated time between taps on the previously seen taps.
        // That makes sure we react to double clicks as soon as possible, but
        // we also give enough time to press a third time, if the user is on
        // a tapping streak.
        const int dtTmo = keypad.startOfDim - keypad.firstTap;
        // We don't really know yet whether this will be a double tap that
        // jumps the level, or whether there will be additional taps to
        // manually step through several discrete brightness values. Update
        // our own representation, as that's safe to do synchronously but
        // don't send a command to the Lutron device as asynchronous execution
        // can make brightness jump back and forth.
        for (const auto& [id, level] : targetLevels) {
          setDMXorLutron(id, level, true, true, true);
        }
        event_.addTimeout(dtTmo,
        [this, jumpedLevel, targetLevels, &keypad]() {
          if (keypad.numTaps == 2) {
            // This was a doubletap. It wasn't followed by any other taps. So,
            // execute the action and reset tap detection.
            keypad.numTaps = 0;
            keypad.firstTap = 0;
            for (const auto& as :
                   keypad.components[keypad.lastButton].assignments) {
              setDMXorLutron(as.id, jumpedLevel, true, true, false);
            }
          } else {
            // Our doubletap actually turned out to be one in a series of
            // several taps. By the time our timer expired, numTaps was either
            // bigger than 2 or had possibly already reset back to 1. In either
            // case, it is extremely unlikely to be exactly 2.
            // We never send our brightness level to Lutron. But that's OK, as
            // one of the subsequent button taps will have done that with a more
            // up-to-date value. The value captured by this lambda is now stale.
            // So, there literally is nothing to do for us.
          }
        });
      } else {
        // If there is no disambiguity about whether this might be a doubletap,
        // we can immediately set the final target level. In that case, we
        // also send the command to Lutron.
        for (const auto& [id, level] : targetLevels) {
          setDMXorLutron(id, level, true, true, false);
        }
      }

      keypad.startOfDim = now;
      keypad.startingLevels.clear();
    } else {
      auto newDirection = button.type == BUTTON_LOWER ? -1 : 1;
      if (keypad.dimDirection != newDirection) {
        keypad.dimDirection = newDirection;
        keypad.startOfDim = 0;
        keypad.firstTap = 0;
        keypad.numTaps = 0;
      }
      keypad.startingLevels.clear();

      // We want to disambiguate double taps from a stream of multiple taps.
      // If we are seeing the latter, just keep counting and don't reset to
      // single tap mode.
      if (keypad.startOfDim && now - keypad.startOfDim <= 5000) {
        keypad.numTaps++;
      } else {
        keypad.numTaps = 1;
      }
      keypad.startOfDim = now;
      if (keypad.numTaps == 1) {
        keypad.firstTap = now;
      }

      for (const auto& as : keypad.components[keypad.lastButton].assignments){
        // Both DMX outputs defined in "site.json" and defined inline with a
        // dummy device must be manually adjusted while the dimmer button is
        // being held.
        if (as.level) {
          if (as.id < 0) {
            if (-as.id <= (int)namedOutput_.size()) {
              keypad.startingLevels[as.id] = namedOutput_[-as.id-1].level;
            }
          } else {
            auto it = outputs_.find(as.id);
            if (it != outputs_.end() &&
                it->second.name.find(':') != std::string::npos) {
              keypad.startingLevels[as.id] = it->second.level;
              suppressLutronDimmer(as.id, true);
            }
          }
        }
      }
      if (keypad.startingLevels.size()) {
        command("", [&](auto) { dimSmooth(keypad); });
      }
    }
    break; }

  // There are other button types that we don't support.
  default:
    DBG("Don't know what to do about button \"" << button.name << "\"");
    break;
  }
}

void RadioRA2::dimSmooth(Device& keypad) {
//DBG("RadioRA2::dimSmooth()");
  if (keypad.startingLevels.size() <= 0) {
    return;
  }
  int delta = ((int)Util::millis() - keypad.startOfDim)*
               (int)DIMRATE/10*keypad.dimDirection;
  for (auto it = keypad.startingLevels.begin();
       it != keypad.startingLevels.end();) {
    // Step each fixtures brightness level by delta and update both our
    // internal cached value and the DMX setting.
    const auto& id = it->first;
    const int level = std::min(10000, std::max(0, it->second + delta));
    setDMXorLutron(id, level, false, true, true);

    // If we hit the maximum position, stop adjusting the DMX fixture.
    if (level == 0 || level == 10000) {
      it = keypad.startingLevels.erase(it);
    } else {
      ++it;
    }
  }
  event_.addTimeout(50, [&]() { dimSmooth(keypad); });
}
