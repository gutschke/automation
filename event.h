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
  void *addPollFd(int fd, short events, std::function<bool (pollfd *)> cb);
  bool removePollFd(int fd, short events = 0);
  bool removePollFd(void *handle);
  void *addTimeout(unsigned tmo, std::function<void (void)>);
  bool removeTimeout(void *handle);
  void runLater(std::function<void(void)>);
  void *addLoop(std::function<void (unsigned tmo)> cb);
  void removeLoop(void *handle);

 private:
  struct PollFd {
    PollFd(int fd, short events, std::function<bool (pollfd *)> cb)
      : fd(fd), events(events), cb(cb) { }
    int   fd;
    short events;
    std::function<bool (pollfd *)> cb;
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
  std::vector<PollFd *> freePollFds_;
  std::vector<Timeout *> freeTimeouts_;
  std::vector<std::function<void ()>> later_;
  std::vector<std::function<void (unsigned)> *> loop_;
  pollfd *fds_ = nullptr;
  bool done_ = false;
};
