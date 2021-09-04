#pragma once

#include <sys/socket.h>

#include <functional>
#include <map>
#include <pugixml.hpp>
#include <string>
#include <vector>

#include "event.h"
#include "lutron.h"

class RadioRA2 {
 public:
  enum DeviceType {
    DEV_UNKNOWN,
    DEV_PICO_KEYPAD,
    DEV_SEETOUCH_KEYPAD,
    DEV_HYBRID_SEETOUCH_KEYPAD,
    DEV_MOTION_SENSOR,
    DEV_MAIN_REPEATER
  };

  inline static const std::string ALIAS = "RRA2:";
  inline static const std::string DMXALIAS = "DMX:";

  RadioRA2(Event& event,
           std::function<void ()> init = nullptr,
           std::function<void (const std::string& line,
                               const std::string& context)> input = nullptr,
           std::function<void (int, int, bool)> ledState = nullptr,
           std::function<void ()> hb = nullptr,
           std::function<void ()> schemaInvalid = nullptr,
           const std::string& gateway = "",
           const std::string& username = "",
           const std::string& password = "");
  ~RadioRA2();
  int addOutput(const std::string name, std::function<void (int)> cb);
  void addToButton(int kp, int bt, int id, int level, bool makeToggle = false);
  void toggleOutput(int out);
  DeviceType deviceType(int id) {
    auto dev = devices_.find(id);
    return dev != devices_.end() ? dev->second.type : DEV_UNKNOWN;
  }
  void command(const std::string& cmd,
               std::function<void (const std::string& res)> cb = [](auto){},
               std::function<void (void)> err = [](){}) {
    lutron_.command(cmd, cb, err);
  }
  std::string getKeypads();

 private:
  const unsigned int SHORT_REOPEN_TMO =  5000;
  const unsigned int LONG_REOPEN_TMO  = 60000;
  const unsigned int ALIVE_INTERVAL   = 60000;
  const unsigned int ALIVE_CMD_TMO    =  5000;
  const unsigned int DIMLEVELS        =    15;

  enum ActionType {
    ACTION_UNKNOWN        =  0,
    ACTION_ENABLE         =  1,
    ACTION_DISABLE        =  2,
    ACTION_PRESS          =  3,
    ACTION_RELEASE        =  4,
    ACTION_HOLD           =  5,
    ACTION_DOUBLETAP      =  6,
    ACTION_SCENE          =  7,
    ACTION_LEDSTATE       =  9,
    ACTION_LIFTTILTLEVEL  = 10,
    ACTION_STARTRAISTILT  = 11,
    ACTION_STARTLOWERTILT = 12,
    ACTION_STOPTILT       = 13,
    ACTION_STARTRAISLIFT  = 14,
    ACTION_STARTLOWERLIFT = 15,
    ACTION_STOPLIFT       = 16,
    ACTION_LIGHTLEVEL     = 14,
    ACTION_ZONELOCK       = 15,
    ACTION_SCENELOCK      = 16,
    ACTION_SEQUENCE       = 17,
    ACTION_STARTRAISE     = 18,
    ACTION_STARTLOWER     = 19,
    ACTION_DMX            = 17,
    ACTION_JOGRAISE       = 18,
    ACTION_JOGLOWER       = 19,
    ACTION_JOG4RAISE      = 20,
    ACTION_JOG4LOWER      = 21,
    ACTION_STOPRAISELOWER = 20,
    ACTION_BATTERYSTATUS  = 22,
    ACTION_LIFTTILT       = 23,
    ACTION_LIFT           = 24,
    ACTION_TILT           = 25,
    ACTION_HOLDRELEASE    = 32,
    ACTION_TIMECLOCK      = 34,
    ACTION_CCISTATE       = 35,
    ACTION_ACTIVELED      = 36,
    ACTION_INACTIVELED    = 37,
    ACTION_FAHRENHEIT     = 41,
    ACTION_CELSIUS        = 42
  };

  enum MonitorType {
    MONITOR_DIAGNOSTICS = 1,
    MONITOR_EVENT       = 2,
    MONITOR_BUTTON      = 3,
    MONITOR_LED         = 4,
    MONITOR_ZONE        = 5,
    MONITOR_OCCUPANCY   = 6,
    MONITOR_PHOTOSENSOR = 7,
    MONITOR_SCENE       = 8,
    MONITOR_SYSVAR      = 10,
    MONITOR_REPLYSTATE  = 11,
    MONITOR_PROMPTSTATE = 12,
    MONITOR_OCCUPANCYGRP= 13,
    MONITOR_DEVICELOCK  = 14,
    MONITOR_SEQUENCE    = 16,
    MONITOR_HVAC        = 17,
    MONITOR_MODE        = 18,
    MONITOR_SHADEGRP    = 23,
    MONITOR_PARTWALL    = 24,
    MONITOR_TEMPERATURE = 27,
    MONITOR_ALL         = 255
  };

  enum LedLogic {
    LED_UNKNOWN     =  0,

    // LED is on when at least one device is at any level
    LED_MONITOR     =  1, // Toggle control / Room monitoring

    // LED is on when all devices are at the exact programmed level
    LED_SCENE       =  2, // Single/Multi-room scene

    // Doesn't affect LED status
    LED_RAISELOWER  =  4, // Raise/lower last button pressed

    // LED turns on when the programmed shades are moving
    LED_SHADETOGGLE = 11  // Shade toggle
  };

  enum ButtonType {
    /* 0 */ BUTTON_UNKNOWN,
    /* 1 */ BUTTON_TOGGLE,          // Toggle control / Room monitoring
    /* 2 */ BUTTON_ADVANCED_TOGGLE, // Shade tilt view toggle
    /* 3 */ BUTTON_SINGLE_ACTION,   // Single/Multi-room scene
    /* 4 */ BUTTON_LOWER,           // Lower last button pressed
    /* 5 */ BUTTON_RAISE            // Raise last button pressed
  };

  struct Assignment {
    Assignment(int id, int level)
      : id(id), level(level) { }
    bool operator==(const Assignment& o) const {
      return id == o.id && level == o.level;
    }
    int id, level;
  };

  struct Component {
    Component() { }
    Component(int id, int led, const std::string& name,
              LedLogic logic, ButtonType type)
      : id(id), led(led), name(name), logic(logic), type(type),
        ledState(false), uncertain(false) { }
    bool operator==(const Component& o) const {
      if (id == o.id && led == o.led && logic == o.logic && name == o.name) {
        // We use the comparison operator to check whether the schema on the
        // Lutron controller is identical with our cached copy. Most of our
        // internal representation stays constant. But there is one notable
        // exception. addToButton() allows for updating the button assignments
        // with additional rules for the DMX light fixtures. We have to filter
        // out these changes before we can do that comparison.
        auto pred = [](const Assignment& a) { return a.id >= 0; };
        std::vector<Assignment> a, b;
        copy_if(  assignments.begin(),   assignments.end(),
                std::back_inserter(a), pred);
        copy_if(o.assignments.begin(), o.assignments.end(),
                std::back_inserter(b), pred);
        return (type == o.type ||
                (a.empty() &&   type == BUTTON_TOGGLE)  ||
                (b.empty() && o.type == BUTTON_TOGGLE)) &&
               a == b;
      }
      return false;
    }
    int                     id, led;
    std::string             name;
    LedLogic                logic;
    ButtonType              type;
    std::vector<Assignment> assignments;
    bool                    ledState;
    bool                    uncertain;
  };

  struct Device {
    Device() { }
    Device(int id, const std::string& name, DeviceType type)
      : id(id), name(name), type(type), lastButton(-1) { }
    bool operator==(const Device& o) const {
      return id == o.id && type == o.type && name == o.name &&
             components == o.components;
    }
    int                      id;
    std::string              name;
    DeviceType               type;
    std::map<int, Component> components;
    int                      lastButton;
  };

  struct Output {
    Output() { }
    Output(int id, const std::string& name, bool dim)
      : id(id), name(name), dim(dim), level(0) { }
    bool operator==(const Output& o) const {
      return id == o.id && dim == o.dim && name == o.name;
    }
    int         id;
    std::string name;
    bool        dim;
    int         level;
  };

  void healthCheck();
  void readLine(const std::string& line);
  void init(std::function<void (void)> cb);
  void closed();
  void getSchema(const sockaddr& addr, socklen_t len, std::function<void ()>cb);
  static int strToLevel(const char *ptr);
  bool extractSchemaInfo(pugi::xml_document& xml_);
  void refreshCurrentState(std::function<void ()> cb);
  int getCurrentLevel(int id);
  void recomputeLEDs();
  void buttonPressed(Device& keypad, Component& button, bool isReleased);

  Event& event_;
  Lutron lutron_;
  bool initialized_;
  std::function<void (const std::string&, const std::string&)> input_;
  std::function<void (int, int, bool)> ledState_;
  std::function<void ()> hb_;
  std::function<void ()> schemaInvalid_;
  std::vector<std::function<void ()>> onInit_;
  void *recompute_;
  unsigned int reconnect_;
  unsigned int checkStarted_;
  unsigned int checkFinished_;
  unsigned int uncertain_;
  int schemaSock_;
  std::map<int, Device> devices_;
  std::map<int, Output> outputs_;
  std::vector<std::tuple<std::string /* unique name */, int /* current level */,
                         std::function<void (int)>>> namedOutput_;
};
