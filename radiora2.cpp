#include <arpa/inet.h>
#include <errno.h>
#include <locale.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <fmt/format.h>
#include <iostream>
#include <sstream>

#include "lutron.h"
#include "radiora2.h"
#include "util.h"


RadioRA2::RadioRA2(Event& event,
                   std::function<void ()> init,
                   std::function<void (const std::string& line,
                                       const std::string& context)> input,
                   std::function<void (int, int, bool)> ledState,
                   std::function<void ()> hb,
                   std::function<void ()> schemaInvalid,
                   const std::string& gateway,
                   const std::string& username,
                   const std::string& password)
  : event_(event),
    lutron_(event,
            [this](const std::string& line) { readLine(line); },
            [this](auto cb) { RadioRA2::init(cb); },
            [this]() { closed(); },
            gateway, username, password),
    initialized_(false),
    input_(input),
    ledState_(ledState),
    hb_(hb),
    schemaInvalid_(schemaInvalid),
    recompute_(0),
    reconnect_(SHORT_REOPEN_TMO),
    checkStarted_(0),
    checkFinished_(0),
    uncertain_(0),
    schemaSock_(-1) {
  setlocale(LC_NUMERIC, "C");
  onInit_.push_back(init);

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
            lutron_.command(fmt::format("#DEVICE,{},{},9,{}",
              btn.id, btn.led, btn.ledState ? 1 : 0));
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
  DBG("Read line: \"" << line << "\"");
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
              ledState_(keypad.id, led->second.id, *endPtr == '1');
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
    const auto& out = outputs_.find(id);
    if (out != outputs_.end() &&
        *endPtr++ == ',' && *endPtr++ == '1' && *endPtr++ == ',') {
      // Update our internal state.
      out->second.level = strToLevel(endPtr);
      context = out->second.name;

      // Check if there is any aliased output. This allows us to take over
      // the implementation of an output that is *also* natively handled by
      // the Lutron controller.
      const std::string& alias = fmt::format("{}{}", ALIAS, id);
      const std::string& dmxalias = fmt::format("{}{}", DMXALIAS, id);
      for (auto& [name, level, cb] : namedOutput_) {
        if (name == alias || name == dmxalias) {
          if (level != out->second.level) {
            event_.runLater([=]() { cb(out->second.level); });
          }
          level = out->second.level;
        }
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
  if (input_) {
    input_(line, Util::trim(context));
  }
}

void RadioRA2::init(std::function<void (void)> cb) {
  DBG("Connection opened");
  // Sanity check. If we never actually succeeded in opening the connection,
  // close it down properly (if not already done) and notify our caller.
  if (!lutron_.isConnected()) {
    lutron_.closeSock();
    cb();
    return;
  }

  // Initialize the connection. Most notably, that means turning on all
  // the notifications that we are interested in.
  static const MonitorType events[] = {
    MONITOR_BUTTON, MONITOR_LED, MONITOR_OCCUPANCY, MONITOR_PHOTOSENSOR };
  for (const auto& ev : events) {
    lutron_.command(fmt::format("#MONITORING,{},1", ev));
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
        // there will be onInit_ handlers that need to be notified. Remove them
        // afterwards.
        const auto onInit = std::move(onInit_);
        for (const auto& o : onInit) {
          event_.runLater(o);
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
  if ((devices_.size() || outputs_.size()) && schemaInvalid_) {
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
        component.node().select_nodes(".//PresetAssignment");
      for (const auto& assign : assignments) {
        // Convert dimmer level to our internal representation.
        Assignment as(atoi(assign.node().child_value("IntegrationID")),
                      strToLevel(assign.node().child_value("Level")));
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
               output.node().attribute("Name").value(),
               output.node().attribute("OutputType").value() !=
               std::string("NON_DIM"));
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
    lutron_.command(fmt::format("?OUTPUT,{},1", out.second.id),
                    [this](auto) { lutron_.initStillWorking(); });
  }

  // Invoke our callback, when all pending commands have completed. This is
  // much easier than explicitly keeping track of a chain of callbacks.
  // We don't actually wait for the LED status to be updated. Overall,
  // intialization is quite slow because of all the communication involved.
  // Speculatively initialize things as fast as we can, and then fix things
  // up asynchronously as needed.
  lutron_.command("", [cb, this](auto) {
    cb();
    event_.addTimeout(2000, [this]() {
      for (auto& dev : devices_) {
        for (auto& comp : dev.second.components) {
          if (comp.second.led < 0) {
            continue;
          }
          // LED state isn't always perfectly tracked by the Lutron controller.
          // Besides 0 and 1, it can also return 255. In that case, we assume
          // that the LED is unchanged or turned off. After a fresh restart,
          // initialize it to "off". That might or might not be correct
          // depending on why the controller lost track of the LED state.
          // Hopefully, at that point, RadioRA2::recomputeLEDs() will
          // eventually bring everything back to a consistent state.
          if (ledState_ &&
              (dev.second.type == DEV_SEETOUCH_KEYPAD ||
               dev.second.type == DEV_HYBRID_SEETOUCH_KEYPAD)) {
            ledState_(dev.second.id, comp.second.id, false);
          }
          comp.second.ledState = 0;
          lutron_.command(fmt::format("?DEVICE,{},{},{}",
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
    return std::min(std::max(std::get<1>(namedOutput_[-id-1]), 0), 10000);
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
          if (component.logic == LED_MONITOR && level > 0) {
            // LED is on when at least one device is at any level
            ledState = true;
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
            ledState_(device.id, component.id, ledState);
          }
          lutron_.command(fmt::format("#DEVICE,{},{},9,{}",
            device.id, component.led, ledState ? 1 : 0));
        }
      }
    }
  }
}

int RadioRA2::addOutput(const std::string name, std::function<void (int)> cb) {
  // Returns a previously registered virtual output device that is identified
  // by its unique name. If no such device exists, register a new one and
  // assign it an id number. This number is always negative to distinguish it
  // from Lutron integration ids.
  int id;
  const auto& it = std::find_if(namedOutput_.begin(), namedOutput_.end(),
                         [&](const auto &n) { return std::get<0>(n) == name; });
  if (it == namedOutput_.end()) {
    namedOutput_.push_back(make_tuple(name, 0, cb));
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
  devices_[kp].components[bt].assignments.push_back(Assignment(id, level*100));
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

void RadioRA2::buttonPressed(Device& keypad, Component& button,
                             bool isReleased) {
  // Replicate the same logic that Lutron does for button presses, but
  // apply it to non-Lutron virtual outputs (e.g. DMX fixtures).
  switch (button.type) {
  // Toggle control / Room monitoring
  case BUTTON_TOGGLE:
  case BUTTON_ADVANCED_TOGGLE: {
    keypad.lastButton = button.id;
    if (isReleased) {
      return;
    }
    bool on = false;
    for (const auto& as : button.assignments) {
      on |= getCurrentLevel(as.id) > 0;
    }
    for (const auto& as : button.assignments) {
      if (as.id < 0) {
        std::get<1>(namedOutput_[-as.id-1]) = on ? 0 : as.level;
        std::get<2>(namedOutput_[-as.id-1])(on ? 0 : as.level);
      }
    }
    break; }

  // Single/Multi-room scene
  case BUTTON_SINGLE_ACTION:
    keypad.lastButton = button.id;
    if (isReleased) {
      return;
    }
    for (const auto& as : button.assignments) {
      if (as.id < 0) {
        std::get<1>(namedOutput_[-as.id-1]) = as.level;
        std::get<2>(namedOutput_[-as.id-1])(as.level);
      }
    }
    break;

  // Dimmer control buttons
  case BUTTON_LOWER:
  case BUTTON_RAISE: {
    if (isReleased) {
      return;
    }
    if (keypad.lastButton < 0 ||
        keypad.components.find(keypad.lastButton) == keypad.components.end()) {
      DBG("No last button known for this keypad");
      return;
    }
    for (const auto& as : keypad.components[keypad.lastButton].assignments) {
      if (as.id < 0) {
        auto& level = std::get<1>(namedOutput_[-as.id-1]);
        if (button.type == BUTTON_LOWER) {
          level = std::max(0u,
                           ((DIMLEVELS*level+5000)/10000-1)*10000/DIMLEVELS);
        } else {
          level = std::min(10000u,
                           ((DIMLEVELS*level+5000)/10000+1)*10000/DIMLEVELS);
        }
        std::get<2>(namedOutput_[-as.id-1])(level);
      }
    }
    break; }

  // There are other button types that we don't support.
  default:
    DBG("Don't know what to do about button \"" << button.name << "\"");
    break;
  }
}

std::string RadioRA2::getKeypads() {
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
  str << "{";
  // Iterate over all devices, but only return information for actual keypads.
  // Most notably, this skips over the virtual buttons associated with the
  // Lutron controller itself.
  bool firstKeypad = true;
  for (auto device = devices_.begin(); device != devices_.end(); ++device) {
    while (device->second.type != DEV_PICO_KEYPAD &&
           device->second.type != DEV_SEETOUCH_KEYPAD &&
           device->second.type != DEV_HYBRID_SEETOUCH_KEYPAD) {
      if (++device == devices_.end()) {
        goto allDevices;
      }
    }
    // Keeping track of whether to include a trailing "," comma is tedious.
    if (firstKeypad) {
      firstKeypad = false;
    } else {
      str << ",";
    }
    const auto dev = device->second;
    // Output the name of the keypad, the LEDs and the buttons.
    str << fmt::format("\"{}\":{{\"label\":\"{}\",\"leds\":{{",
                       dev.id, esc(dev.name));
    bool firstLed = true;
    for (auto button = dev.components.begin(); button != dev.components.end();
         ++button) {
      while (button->second.led < 0) {
        if (++button == dev.components.end()) {
          goto allLeds;
        }
      }
      if (firstLed) {
        firstLed = false;
      } else {
        str << ",";
      }
      const auto btn = button->second;
      str << fmt::format("\"{}\":{}", btn.id, (int)btn.ledState);
    }
  allLeds:
    str << "},\"buttons\":{";
    for (auto button = dev.components.begin(); button != dev.components.end();){
      const auto btn = button->second;
      str << fmt::format("\"{}\":", btn.id);
      // Dimmer buttons are encoded as booleans to make them easy to
      // identify. Other buttons are stored with their label.
      if (btn.type == BUTTON_LOWER || btn.type == BUTTON_RAISE) {
        str << (btn.type != BUTTON_LOWER ? "true" : "false");
      } else {
        str << fmt::format("\"{}\"", esc(btn.name));
      }
      str << (++button != dev.components.end() ? "," : "");
    }
    str << "}}";
  }
 allDevices:
  str << "}";
  return str.str();
}
