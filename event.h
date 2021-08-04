#pragma once

#include <poll.h>

#include <functional>
#include <vector>

class Event {
 public:
  Event();
  ~Event();
  void loop();
  void exitLoop();
  void *addPollFd(int fd, short events, std::function<bool (void)> cb);
  bool removePollFd(int fd, short events = 0);
  bool removePollFd(void *handle);
  void *addTimeout(unsigned tmo, std::function<void (void)>);
  bool removeTimeout(void *handle);
  void runLater(std::function<void(void)>);

 private:
  struct PollFd {
    PollFd(int fd, short events, std::function<bool (void)> cb)
      : fd(fd), events(events), cb(cb) { }
    int   fd;
    short events;
    std::function<bool (void)> cb;
  };

  struct Timeout {
    Timeout(unsigned tmo, std::function<void (void)> cb)
      : tmo(tmo), cb(cb) { }
    unsigned tmo;
    std::function<void (void)> cb;
  };

  void handleTimeouts(unsigned now);
  void recomputeTimeoutsAndFds();

  std::vector<PollFd *> pollFds_, *newFds_ = nullptr;
  std::vector<Timeout *> timeouts_, *newTimeouts_ = nullptr;
  std::vector<std::function<void (void)> > later_;
  struct ::pollfd *fds_ = nullptr;
  bool done_ = false;
};
