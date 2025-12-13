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
      : fd(fd), events(events), cb(cb), deleted(false) { }
    int   fd;
    short events;
    std::function<bool (pollfd *)> cb;
    bool  deleted;
  };

  struct Timeout {
    Timeout(unsigned tmo, std::function<void (void)> cb)
      : tmo(tmo), cb(cb), deleted(false) { }
    unsigned tmo;
    std::function<void (void)> cb;
    bool deleted;
  };

  void handleTimeouts(unsigned now);
  void compact();

  std::vector<PollFd *> pollFds_;
  std::vector<PollFd *> addedFds_;

  std::vector<Timeout *> timeouts_;
  std::vector<Timeout *> addedTimeouts_;

  std::vector<std::function<void ()>> later_;
  std::vector<std::function<void (unsigned)> *> loop_;

  pollfd *fds_ = nullptr;
  size_t fds_capacity_ = 0;

  bool done_ = false;
  bool processing_ = false;
};
