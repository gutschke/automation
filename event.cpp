#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <algorithm>
#include <signal.h>

#include "event.h"
#include "util.h"


Event::Event() {
}

Event::~Event() {
  compact();

  // Clean up remaining active objects
  for (auto p : pollFds_) delete p;
  for (auto p : addedFds_) delete p;
  for (auto t : timeouts_) delete t;
  for (auto t : addedTimeouts_) delete t;

  delete[] fds_;
  for (auto l : loop_) delete l;
}

void Event::loop() {
  compact();

  while (!done_ && (!pollFds_.empty() || !timeouts_.empty() ||
                    !later_.empty() || !addedFds_.empty() ||
                    !addedTimeouts_.empty())) {

    // Calculate timeout
    unsigned now = Util::millis();
    unsigned tmo = later_.empty() ? 0 : now + 1;

    for (const auto& timeout : timeouts_) {
      if (!timeout->deleted && (!tmo || tmo > timeout->tmo)) {
        tmo = timeout->tmo;
      }
    }
    // Also check added timeouts (though they don't usually expire immediately)
    for (const auto& timeout : addedTimeouts_) {
      if (!tmo || tmo > timeout->tmo) {
        tmo = timeout->tmo;
      }
    }

    if (tmo) {
      if (tmo <= now || !later_.empty()) {
        handleTimeouts(now);
        compact();
        continue;
      } else {
        tmo -= now;
      }
    }

    // Loop callbacks
    if (loop_.size()) {
      for (auto l : loop_) {
        (*l)(tmo);
      }
      // If loop callbacks modified state, compact before polling
      if (!addedFds_.empty() || !addedTimeouts_.empty()) {
        compact();
      }
    }

    // Prepare pollfd array. Reallocate if capacity is insufficient
    if (pollFds_.size() > fds_capacity_) {
        delete[] fds_;
        fds_capacity_ = pollFds_.size() + 16; // Small padding
        fds_ = new pollfd[fds_capacity_];
    }

    int nFds = 0;
    for (auto p : pollFds_) {
        if (!p->deleted) {
            fds_[nFds].fd = p->fd;
            fds_[nFds].events = p->events;
            fds_[nFds].revents = 0;
            nFds++;
        }
    }

    // Poll
    timespec ts = { (long)tmo / 1000L, ((long)(tmo % 1000))*1000000L };
    int rc = ppoll(fds_, nFds, tmo ? &ts : nullptr, nullptr);

    // Dispatch
    if (rc == 0) {
        handleTimeouts(Util::millis());
    } else if (rc > 0) {
        processing_ = true;
        int k = 0;
        for (auto p : pollFds_) {
            if (p->deleted) continue;

            if (fds_[k].revents) {
                if (p->cb && !p->cb(&fds_[k])) {
                    p->deleted = true;
                }
                rc--;
            }
            k++;
            // Optimization: stop if we processed all reported events
            if (rc == 0) break;
        }
        processing_ = false;
    }

    compact();
  }
}

void Event::exitLoop() {
  done_ = true;
}

void *Event::addPollFd(int fd, short events, std::function<bool (pollfd*)> cb) {
  PollFd *pfd = new PollFd(fd, events, cb);
  if (processing_) {
      addedFds_.push_back(pfd);
  } else {
      pollFds_.push_back(pfd);
  }
  return pfd;
}

bool Event::removePollFd(int fd, short events) {
  bool removed = false;
  auto remove = [&](std::vector<PollFd*>& vec) {
      for (auto p : vec) {
          if (!p->deleted && p->fd == fd && (!events || events == p->events)) {
              p->deleted = true;
              removed = true;
          }
      }
  };

  remove(pollFds_);
  if (processing_) remove(addedFds_);

  return removed;
}

bool Event::removePollFd(void *handle) {
  if (!handle) return false;

  bool removed = false;
  auto p = static_cast<PollFd*>(handle);

  if (!p->deleted) {
      p->deleted = true;
      removed = true;
  }
  return removed;
}

void *Event::addTimeout(unsigned tmo, std::function<void (void)> cb) {
  Timeout *t = new Timeout(tmo + Util::millis(), cb);
  if (processing_) {
      addedTimeouts_.push_back(t);
  } else {
      timeouts_.push_back(t);
  }
  return t;
}

bool Event::removeTimeout(void *handle) {
  if (!handle) return false;
  auto t = static_cast<Timeout*>(handle);

  if (!t->deleted) {
      t->deleted = true;
      return true;
  }
  return false;
}

void Event::handleTimeouts(unsigned now) {
  processing_ = true;

  // Process existing timeouts
  for (const auto& timeout : timeouts_) {
    if (!timeout->deleted && now >= timeout->tmo) {
      const auto cb = std::move(timeout->cb);
      timeout->deleted = true;
      if (cb) {
        cb();
      }
    }
  }

  // Process "later_" callbacks
  while (!later_.empty()) {
      const auto later = std::move(later_);
      // "later_" is now empty, ready for new insertions
      for (const auto& cb : later) {
          if (cb) cb();
      }
  }

  processing_ = false;
}

void Event::runLater(std::function<void(void)> cb) {
  later_.push_back(cb);
}

void *Event::addLoop(std::function<void (unsigned)> cb) {
  loop_.push_back(new std::function<void (unsigned)>{cb});
  return loop_.back();
}

void Event::removeLoop(void *handle) {
  auto cb = (std::function<void (unsigned)> *)handle;
  loop_.erase(std::remove(loop_.begin(), loop_.end(), cb));
  delete cb;
}

void Event::compact() {
  // Compact pollfds
  if (!pollFds_.empty()) {
      pollFds_.erase(std::remove_if(pollFds_.begin(), pollFds_.end(),
        [](PollFd* p) {
          if (p->deleted) {
              delete p;
              return true;
          }
          return false;
      }), pollFds_.end());
  }

  if (!addedFds_.empty()) {
      pollFds_.insert(pollFds_.end(), addedFds_.begin(), addedFds_.end());
      addedFds_.clear();
  }

  // compact timeouts
  if (!timeouts_.empty()) {
      timeouts_.erase(std::remove_if(timeouts_.begin(), timeouts_.end(),
        [](Timeout* t) {
          if (t->deleted) {
              delete t;
              return true;
          }
          return false;
      }), timeouts_.end());
  }

  if (!addedTimeouts_.empty()) {
      timeouts_.insert(timeouts_.end(), addedTimeouts_.begin(),
                       addedTimeouts_.end());
      addedTimeouts_.clear();
  }
}
