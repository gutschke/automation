#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <signal.h>

#include "event.h"
#include "util.h"


Event::Event() {
}

Event::~Event() {
  recomputeTimeoutsAndFds();
  while (!later_.empty()) {
    // There could be critical clean-up happening as part of the
    // later_ callbacks. Better call these, even though we are in the
    // process of shutting down.
    const auto later = std::move(later_);
    for (const auto& cb : later) {
      cb();
    }
  }
  // Ideally, the caller should ensure that there are no unresolved
  // pending tasks. But if there are, we'll abandon them. Hopefully, that's
  // OK and they didn't involve any dangling objects.
  for (const auto& timeout : timeouts_) {
    delete timeout;
  }
  for (const auto& pollFd : pollFds_) {
    delete pollFd;
  }
  delete[] fds_;
  for (auto l : loop_) {
    delete l;
  }
}

void Event::loop() {
  recomputeTimeoutsAndFds();
  while (!done_ && (!pollFds_.empty() || !timeouts_.empty() ||
                    !later_.empty())) {
    // Find timeout that will fire next, if any
    unsigned now = Util::millis();
    unsigned tmo = later_.empty() ? 0 : now + 1;
    for (const auto& timeout : timeouts_) {
      if (timeout && (!tmo || tmo > timeout->tmo)) {
        tmo = timeout->tmo;
      }
    }
    if (tmo) {
      // If the timeout has already expired, handle it now
      if (tmo <= now || !later_.empty()) {
        handleTimeouts(now);
        continue;
      } else {
        tmo -= now;
      }
    }

    // Some users want to be invoked each time the loop iterates. This
    // would give them the opportunity to adjust the next timeout value
    // right before it normally fires.
    if (loop_.size()) {
      for (auto l : loop_) {
        (*l)(tmo);
      }
      if (newTimeouts_) {
        recomputeTimeoutsAndFds();
        continue;
      }
    }

    // Wait for next event
    timespec ts = { (long)tmo / 1000L, ((long)(tmo % 1000))*1000000L };
    int nFds = pollFds_.size();
    int rc = ppoll(fds_, nFds, tmo ? &ts : nullptr, nullptr);
    if (!rc) {
      handleTimeouts(Util::millis());
    } else if (rc > 0) {
      int i = 0;
      for (auto it = pollFds_.begin();
           rc > 0 && it != pollFds_.end(); it++, i++) {
        if (fds_[i].revents) {
          if (*it) {
            if (!(*it)->cb(&fds_[i])) {
              removePollFd(*it);
            }
          }
          fds_[i].revents = 0;
          rc--;
        }
      }
    }
    recomputeTimeoutsAndFds();
  }
}

void Event::exitLoop() {
  done_ = true;
}

void *Event::addPollFd(int fd, short events, std::function<bool (pollfd*)> cb) {
  if (!newFds_) {
    newFds_ = new std::vector<PollFd *>(pollFds_);
  }
  for (const auto& newFd : *newFds_) {
    if (newFd->fd == fd && !!(newFd->events & events)) {
      DBG("Internal error; adding duplicate event");
      abort();
    }
  }
  PollFd *pfd = new PollFd(fd, events, cb);
  newFds_->push_back(pfd);
  return pfd;
}

bool Event::removePollFd(int fd, short events) {
  bool removed = false;

  // Create vector with future poll information
  if (!newFds_) {
    newFds_ = new std::vector<PollFd *>(pollFds_);
  }
  // Zero out existing record. This avoids the potential for races
  for (auto& pollFd : pollFds_) {
    if (pollFd && fd == pollFd->fd && (!events || events == pollFd->events)) {
      pollFd = nullptr;
      removed = true;
    }
  }
  // Remove fd from future list
  newFds_->erase(std::remove_if(newFds_->begin(), newFds_->end(),
                                [&](auto e) {
    if (fd == e->fd && (!events || events == e->events)) {
      removed = true;
      // It is common for removePollFd() to be called by the callback.
      // But that lambda object includes a lot of state that cannot safely
      // be destroyed while the callback is running. Push the actual
      // destruction onto the "later_" callback.
      runLater([=]() { delete e; });
      return true;
    }
    return false;
  }), newFds_->end());
  return removed;
}

bool Event::removePollFd(void *handle) {
  if (!handle) {
    return false;
  }

  bool removed = false;

  // Create vector with future poll information
  if (!newFds_) {
    newFds_ = new std::vector<PollFd *>(pollFds_);
  }
  // Zero out existing record. This avoids the potential for races
  for (auto& pollFd : pollFds_) {
    if (pollFd && pollFd == handle) {
      pollFd = nullptr;
      removed = true;
    }
  }
  // Remove fd from future list
  newFds_->erase(std::remove_if(newFds_->begin(), newFds_->end(),
                                [&](auto e) {
    if (e == handle) {
      removed = true;
      // It is common for removePollFd() to be called by the callback.
      // But that lambda object includes a lot of state that cannot safely
      // be destroyed while the callback is running. Push the actual
      // destruction onto the "later_" callback.
      runLater([=]() { delete e; });
      return true;
    }
    return false;
  }), newFds_->end());
  return removed;
}

void *Event::addTimeout(unsigned tmo, std::function<void (void)> cb) {
  if (!newTimeouts_) {
    newTimeouts_ = new std::vector<Timeout *>(timeouts_);
  }
  newTimeouts_->push_back(new Timeout(tmo + Util::millis(), cb));
  return newTimeouts_->back();
}

bool Event::removeTimeout(void *handle) {
  if (!handle) {
    return false;
  }

  bool removed = false;

  // Create vector with future timeouts
  if (!newTimeouts_) {
    newTimeouts_ = new std::vector<Timeout *>(timeouts_);
  }
  // Zero out existing record. This avoids the potential for races
  for (auto& timeout : timeouts_) {
    if (timeout == handle) {
      timeout = nullptr;
      removed = true;
    }
  }
  // Remove timeout from future list
  newTimeouts_->erase(std::remove_if(newTimeouts_->begin(),
                                     newTimeouts_->end(),
                                     [&](auto e) {
    if (e == handle) {
      removed = true;
      runLater([=]() { delete e; });
      return true;
    }
    return false;
  }), newTimeouts_->end());
  return removed;
}

void Event::handleTimeouts(unsigned now) {
  do {
    while (!later_.empty()) {
      const auto later = std::move(later_);
      for (const auto& cb : later) {
        cb();
      }
    }
    for (const auto& timeout : timeouts_) {
      if (timeout && now >= timeout->tmo) {
        const auto cb = std::move(timeout->cb);
        removeTimeout(timeout);
        cb();
      }
    }
  } while (!later_.empty());
  recomputeTimeoutsAndFds();
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

void Event::recomputeTimeoutsAndFds() {
  if (newFds_) {
    delete[] fds_;
    fds_ = new pollfd[newFds_->size()];
    int i = 0;
    for (auto it = newFds_->begin(); it != newFds_->end(); it++, i++) {
      fds_[i].fd = (*it)->fd;
      fds_[i].events = (*it)->events;
      fds_[i].revents = 0;
    }
    pollFds_.clear();
    pollFds_.swap(*newFds_);
    delete newFds_;
    newFds_ = nullptr;
  }
  if (newTimeouts_) {
    timeouts_.clear();
    timeouts_.swap(*newTimeouts_);
    delete newTimeouts_;
    newTimeouts_ = nullptr;
  }
}
