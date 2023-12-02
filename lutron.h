#pragma once

#include <sys/socket.h>

#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "event.h"


class Lutron {
 public:
  Lutron(Event& event,
         const std::string& gateway = "",
         const std::string& username = "",
         const std::string& passwd = "");
  ~Lutron();
  Lutron& oninit(std::function<void (std::function<void ()> cb)> init) {
    init_ = init; return *this; }
  Lutron& oninput(std::function<void (const std::string& line)> input) {
    input_ = input; return *this; }
  Lutron& onclosed(std::function<void ()> closed) {
    closed_ = closed; return *this; }

  void command(const std::string& cmd,
               std::function<void (const std::string& res)> cb = [](auto){},
               std::function<void (void)> err = nullptr,
               bool recurs = false);
  void ping(std::function<void (void)> cb = nullptr) {
    command("?SYSTEM,1", cb ? [=](auto) { cb(); }
            : (std::function<void (const std::string&)>)nullptr); }
  void closeSock();
  bool getConnectedAddr(struct sockaddr& addr, socklen_t& len);
  bool isConnected() { return isConnected_; }
  bool commandPending() { return inCommand_; }
  void initStillWorking();

 private:
  const char *PROMPT = "GNET> ";
  const int KEEPALIVE = 5*1000;
  const int TMO = 10*1000;

  void checkDelayed(std::function<void ()> next = nullptr);
  void waitForPrompt(const std::string& prompt,
                     std::function<void (void)> cb,
                     std::function<void (void)> err,
                     bool login = true,
                     std::string::size_type partial = 0);
  void sendData(const std::string& data,
                std::function<void (void)> cb,
                std::function<void (void)> err,
                const std::string& prompt = "GNET> ",
                bool login = true);
  void login(std::function<void (void)> cb,
             std::function<void (void)> err);
  void enterPassword(std::function<void (void)> cb,
                     std::function<void (void)> err);
  void processLine(const std::string& line);
  void readLine();
  void pollIn(std::function<bool (pollfd *)> cb);
  void advanceKeepAliveMonitor();

  struct Command {
    std::string cmd;
    std::function<void (const std::string&)> cb;
    std::function<void ()> err;
  };

  class Timeout {
  public:
    Timeout(Event& event) : event_(event), self_(0), handle_(0) { }
    bool isArmed() { return !!handle_; }
    unsigned next() { return self_ + 1; }
    void set(unsigned tmo, std::function<void (void)> cb);
    void clear();
    unsigned push(std::function<void (void)> cb);
    void pop(unsigned id = 0);
  private:
    Event& event_;
    unsigned self_;
    void *handle_;
    std::vector<std::pair<unsigned, std::function<void (void)>>> finalizers_;
  } timeout_;

  Event& event_;
  std::function<void (const std::string& line)> input_;
  std::function<void (std::function<void ()> cb)> init_;
  std::function<void (void)> closed_;
  std::string gateway_, g_, username_, passwd_;
  int sock_, msock_;
  bool dontfinalize_;
  bool isConnected_;
  bool inCommand_;
  bool inCallback_;
  bool initIsBusy_;
  bool atPrompt_;
  std::string ahead_;
  void *keepAlive_;
  std::vector<Command> later_[2], pending_[2];
  std::vector<std::function<void ()>> onPrompt_;
  struct sockaddr_storage addr_;
  socklen_t addrLen_ = 0;
};
