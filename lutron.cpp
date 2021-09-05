#include <algorithm>
#include <iostream>

#include <errno.h>
#include <netdb.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "lutron.h"
#include "util.h"


Lutron::Lutron(Event& event,
               std::function<void (const std::string& line)> input,
               std::function<void (std::function<void ()> cb)> init,
               std::function<void (void)> closed,
               const std::string& gateway,
               const std::string& username,
               const std::string& passwd)
  : timeout_(event), event_(event), input_(input), init_(init), closed_(closed),
    gateway_(gateway), username_(username.empty() ? "lutron" : username),
    passwd_(passwd.empty() ? "integration" : passwd),
    sock_(-1), dontfinalize_(false), isConnected_(false), inCommand_(false),
    inCallback_(false), atPrompt_(false), keepAlive_(0) {
  DBG("Lutron(\"" << gateway << "\", \"" << username <<"\", \""<<passwd<<"\")");
}

Lutron::~Lutron() {
  DBG("~Lutron()");
  // After the destructor has run, the "closed_()" callback is no longer
  // valid. We suppress execution by setting "isConnected_" to "false".
  // Of course, during normal execution, the destructor shouldn't ever
  // run until the event loop has removed all file descriptor. And that
  // won't happen until after all connections have closed. So, this is all
  // just extra careful code.
  isConnected_ = false;
  closeSock();
}

// We only support a single command at a time. But there are a few situations,
// when new commands get triggered while execution of another command is still
// pending:
//  - if "command()" gets called while "atPrompt_" is false, we first invoke
//    "waitForPrompt()" and then restart "command()" later. This recursive
//    call is valid and shouldn't trip an error. Setting the "recurs" parameter
//    skips some of the initialization and error checking steps that have
//    already happened earlier. This is an internal API. External callers
//    should not set "recurs".
//  - connections can close unexpectedly (e.g. because of networking problems).
//    When the connection is re-opened, it probably needs to be initialized.
//    For example, the gateway has to be informed of the events that we want to
//    monitor. This initialization happens from within the "init_" callback.
//    But that means, we want to be able to recursively submit commands from
//    within the callback and before the outer command completes. This can be
//    allowed by setting the "inCallback_" and temporarily clearing the
//    "inCommand_" flag.
//  - any other concurrently submitted commands will be delayed until they
//    can be executed at a later point in time. There are two queues of
//    commands that will eventually be processed by "scheduleDelayedCommands()"
void Lutron::command(const std::string& cmd,
                     std::function<void (const std::string& res)> cb,
                     std::function<void (void)> err, bool recurs) {
  // "command()" is the main high-level API for interacting with the Lutron
  // gateway. It implements timeouts and enforces that only a single
  // command can be in-flight at any given time.
  if (!recurs) {
    if (commandPending()) {
      later_[inCallback_].push_back(make_tuple(cmd, cb, err));
      return;
    } else {
      inCommand_ = true;
      if (!inCallback_) {
        // Set a timeout for the overall execution of the command. This
        // timeout does not need to be set again for any inferior commands
        // that execute as part of initializing the connection. It should be
        // long enough to cover all of that.
        // But there are situations when initialization can take a really
        // long time. In particular, the Lutron gateway is surprisingly slow
        // in returning the XML schema. So, if we know that we are still
        // positively making progress, extend the timeout. This is signaled by
        // the initIsBusy_ flag.
        initIsBusy_ = false;
        const auto setTmo = Util::rec([=, this](auto&& setTmo) -> void {
          timeout_.set(TMO, [=, this]() {
            if (initIsBusy_) {
              initIsBusy_ = false;
              setTmo();
            } else {
              closeSock();
              err();
            }
          });
        });
        setTmo();
      }
      // Remove the "readLine()" event that collects unsolicited messages.
      event_.removePollFd(sock_);
    }
  }
  if (Util::starts_with(cmd, "?")) {
    pending_[inCallback_].push_back(std::make_tuple(cmd, cb, err));
  } else {
    onPrompt_.push_back([cb]() { cb(""); });
  }
  sendData(
    cmd + "\r\n",
    [this]() { readLine(); },
    [](){},
    PROMPT,
    /* login = */ !inCallback_);
}

// It is safe to call this function multiple times.
void Lutron::closeSock() {
  DBG("Lutron::closeSock(" << (dontfinalize_ ? "dontfinalize" : "") << ")");
  // Closing the connection runs finalizers for all the timeouts and various
  // callbacks that might be pending. This is normally the correct course of
  // action. But if we immediately reopen the socket, trying a different
  // server, then we don't want overall state to be modified. The
  // "dontfinalize_" flag suppresses this behavior.
  onPrompt_.clear();
  bool closing = !dontfinalize_ && isConnected_;
  isConnected_ = false;
  if (!dontfinalize_) {
    inCommand_ = false;
    if (inCallback_) {
      // Only clear delayed commands that accumulated during initialization.
      // Other commands will execute once the connection is re-opened.
      const auto later = std::move(later_[inCallback_]);
      for (const auto& cmd : later) {
        event_.runLater(std::get<2>(cmd));
      }
    }
    // None of the already submitted commands will ever see their
    // exit status, as the connection is now closed. Call their error
    // handlers.
    for (const auto& inCallback : { inCallback_, false }) {
      const auto pending = std::move(pending_[inCallback]);
      for (auto it = pending.rbegin(); it != pending.rend(); ++it) {
        DBG("Failing pending command \"" << std::get<0>(*it) << "\"");
        event_.runLater(std::get<2>(*it));
      }
    }
    inCallback_ = false;
    timeout_.clear();
  }
  // If the underlying file descriptor was still open, close it now and
  // remove it from the event handler.
  if (sock_ >= 0) {
    event_.removePollFd(sock_);
    close(sock_);
    sock_ = -1;
  }
  addrLen_ = 0;
  // If there is a periodic timeout scheduled with the event loop, remove it
  // now. No need to send keep-alive heartbeats, if the connection no longer
  // exists. The higher-level heartbeat will eventually reopen the connection.
  if (keepAlive_) {
    event_.removeTimeout(keepAlive_);
    keepAlive_ = 0;
  }
  // Clear out some of the other state to reset the object, then notify the
  // caller that our socket is now closed.
  atPrompt_ = false;
  ahead_.clear();
  if (closing && closed_) {
    event_.runLater(closed_);
  }

  // Attempt to run delayed commands. This could quite possibly re-open the
  // connection.
  if (!dontfinalize_) {
    checkDelayed();
  }
}

void Lutron::initStillWorking() {
  initIsBusy_ = true;
  if (input_) input_("");
}

bool Lutron::getConnectedAddr(struct sockaddr& addr, socklen_t& len) {
  // The RadioRA2 class retrieves the configuration XML data from the same
  // device that we use for issuing commands. It can retrieve the IP
  // address by calling the getConnectedAddr() method.
  if (!addrLen_ || sizeof(addr_) < addrLen_ || len < addrLen_) {
    len = addrLen_;
    return false;
  }
  memcpy(&addr, &addr_, addrLen_);
  return true;
}

void Lutron::checkDelayed(std::function<void ()> next) {
  // Any command() that arrives when we are not ready to execute it will be
  // delayed until "inCommand_" becomes false. Try to reschedule when
  // possible.
  // There are two separate queues. Commands that were scheduled from the
  // "init_" callback can only execute right after the connection has been
  // opened.
  if (later_[inCallback_].size()) {
    const auto cmd = *later_[inCallback_].begin();
    later_[inCallback_].erase(later_[inCallback_].begin());
    if (!std::get<0>(cmd).size()) {
      // Sometimes, we want to push a callback to the end of the queue, so
      // that it only ever gets executed after all other pending commands
      // have completed. This is done by pushing an empty command string.
      std::get<1>(cmd)("");
    } else {
      // Invoking the callback from Event::runLater() makes sure any global
      // state that our callers are about to modify will have settled.
      event_.runLater([=, this,
                       co = std::get<0>(cmd),
                       cb = std::get<1>(cmd),
                       er = std::get<2>(cmd)]() {
        command(co, cb, er); });
    }
  }
  if (next) {
    if (inCommand_ || later_[inCallback_].size()) {
      // If there still is work to be done, don't invoke our callback
      // just yet.
      later_[inCallback_].push_back(
        make_tuple(std::string(),
                   [=](const std::string& line) { next(); },
                   [](){}));
    } else {
      // All delayed tasks have now completed.
      next();
    }
  }
}

void Lutron::waitForPrompt(const std::string& prompt,
                           std::function<void (void)> cb,
                           std::function<void (void)> err,
                           bool login,
                           std::string::size_type partial) {
  if (atPrompt_ && prompt == PROMPT) {
    // If we already are at the prompt, there is nothing else to do.
    cb();
    return;
  }
  // Set up a timeout that fires if we never see a prompt when we
  // expected one.
  void *tmo = event_.addTimeout(TMO/2, [=, this]() {
    DBG("Timing out on waitForPrompt()");
    for (auto it = pending_[inCallback_].rbegin();
         it != pending_[inCallback_].rend();
         ++it) {
      if (std::get<0>(*it) == prompt) {
        pending_[inCallback_].erase(std::next(it).base());
        break;
      }
    }
    err(); });
  // Remember the callback. It will be invoked when Lutron::processLine()
  // sees the prompt that we are waiting for.
  pending_[inCallback_].push_back(make_tuple(
    prompt,
    [=, this](auto) {
      event_.removeTimeout(tmo);
      cb();
    }, [=, this]() {
      DBG("Failed to find prompt; removing timeout");
      event_.removeTimeout(tmo);
      err();
    }));
  // If the connection is not open yet, go ahead and establish a new
  // connection. This can optionally be suppressed to avoid infinite loops
  // (e.g. when the server isn't available at all).
  if (login) {
    Lutron::login([this]() { readLine(); }, err);
  } else {
    readLine();
  }
}

void Lutron::sendData(const std::string& data,
                      std::function<void (void)> cb,
                      std::function<void (void)> err,
                      const std::string& prompt,
                      bool login) {
  const auto doit = [=, this]() {
    if (!prompt.empty() && (prompt != PROMPT || !atPrompt_)) {
      // We might have to wait for a prompt. This code handles both the
      // regular "GNET> " prompt, but can also deal with the "login: " and
      // "password: " prompts. The latter behave mostly like a normal prompt,
      // with a few minor differences that will be handled by processLine().
      waitForPrompt(prompt, [=, this]() { sendData(data, cb, err, "", false); },
                    err, false);
    } else {
      if (data.empty()) {
        // If there is no data to write then we can return immediately.
        cb();
      } else {
        // Only write data once the socket is ready for writing.
        if (sock_ < 0) {
          err();
          return;
        }
        // We can be called from all sorts of different contexts. Some of these
        // contexts subsequently make global changes such as setting up a
        // POLLIN handler. That's going to confuse our code. So, push onto
        // the Event::runLater() handler to avoid any unexpected state changes.
        event_.runLater([=, this]() {
          event_.removePollFd(sock_);
          event_.addPollFd(sock_, POLLOUT, [=, this](auto) {
            event_.removePollFd(sock_);
            atPrompt_ = false;
            DBG("write(\"" << Util::trim(data) << "\")");
            const auto rc = write(sock_, data.c_str(), data.size());
            if (rc <= 0) {
              // Failed to write any data.
              closeSock();
              err();
            } else if (rc < (decltype(rc))data.size()) {
              // Incomplete write. Keep going.
              sendData(data.substr(rc), cb, err, "", false);
            } else {
              // All done. We have written all data.
              cb();
            }
            return true;
          });
        });
      }
    }
  };
  // If the connection is not open yet, go ahead and establish a new
  // connection. This can optionally be suppressed to avoid infinite loops
  // (e.g. when the server isn't available at all).
  if (login && !inCallback_) {
    Lutron::login(doit, err);
  } else {
    doit();
  }
}

void Lutron::login(std::function<void (void)> cb,
                   std::function<void (void)> err) {
  // If connection is already established, continue using it until it
  // gets closed (e.g. because of an error condition).
  if (sock_ >= 0) {
    cb();
    return;
  }
  DBG("Lutron::login()");
  if (inCallback_) {
    // We are already in the process of handling a login(). Can't be
    // called recursively.
    err();
    return;
  }
  initStillWorking();
  const auto lookupAddress = [=, this](const std::string& gateway) {
    // Look up network address (i.e. resolve DNS names, convert numeric
    // IP addresses to binary representation).
    struct addrinfo hints = { .ai_family = AF_UNSPEC,
                              .ai_socktype = SOCK_STREAM };
    struct addrinfo *result = 0;
    if (getaddrinfo(gateway.c_str(), "23", &hints, &result) || !result) {
      DBG("getaddrinfo() failed (\"" << gateway << "\")");
      closeSock();
      event_.runLater(err);
      return;
    }
    dontfinalize_ = true;
    const auto freeaddrHandler = timeout_.push([this, result]() {
      freeaddrinfo(result);
      dontfinalize_ = false;
    });

    // Iterate over all addresses returned by the resolver and try to connect
    // to the server. This can take a while, so do it asynchronously on the
    // event loop.
    const auto openSocket = Util::rec([=, this](auto&& openSocket,
                                                struct addrinfo *rp) -> void {
      initStillWorking();
      const auto doLogin = [=, this]() {
        inCallback_ = true;
        enterPassword([=, this]() {
          // Pop both tmoHandler and freeaddrHandler.
          addrLen_ = rp->ai_addrlen;
          memcpy(&addr_, rp->ai_addr,
                 std::min((socklen_t)sizeof(addr_), addrLen_));
          timeout_.pop(freeaddrHandler);
          bool oldCommand = inCommand_;
          inCommand_ = false;
          init_([=, this]() {
            checkDelayed([=, this]() {
              DBG("Finished initializing");
              inCallback_ = false;
              inCommand_ = oldCommand && isConnected_;
              cb(); });}); },
        [=, this]() {
          // Pop only tmoHandler (already done), then try next address.
          DBG("Failed to enter password");
          closeSock();
          openSocket(rp->ai_next); });
      };

      // Iterate over all addresses until either we are blocked on I/O or
      // we have reached the end of the list.
      for (;;) {
        // End of list was reached and none of the servers responded. Return
        // error.
        if (!rp) {
          DBG("No addresses found");
          timeout_.pop(freeaddrHandler);
          err();
          return;
        }
        // Create a non-blocking networking socket.
        ahead_.clear();
        sock_ = socket(rp->ai_family,
                       rp->ai_socktype | SOCK_NONBLOCK | SOCK_CLOEXEC,
                       rp->ai_protocol);
        isConnected_ = sock_ >= 0;
        if (isConnected_) {
          // If things take too long, abort the operation.
          const auto tmo = event_.addTimeout(TMO/3,
            [=, this, tmoHandler = timeout_.next()]() {
            DBG("Timeout trying to connect");
            timeout_.pop(tmoHandler);
            closeSock();
            openSocket(rp->ai_next);
          });
          const auto tmoHandler = timeout_.push([event = &event_, tmo]() {
            event->removeTimeout(tmo);
          });
          // Try to open connection to server.
          if (connect(sock_, rp->ai_addr, rp->ai_addrlen) >= 0) {
            // That worked on the first try. Try to log in.
            DBG("Synchronous success");
            timeout_.pop(tmoHandler);
            doLogin();
            return;
          }
          // The connection wasn't immediately available. Wait until the
          // socket becomes writable then check whether the connection has
          // been established asynchronously.
          if (errno == EINPROGRESS || errno == EWOULDBLOCK) {
            event_.addPollFd(sock_, POLLOUT, [=, this](auto) {
              event_.removePollFd(sock_);
              socklen_t addrlen = 0;
              if (getpeername(sock_, (struct sockaddr *)"", &addrlen) < 0) {
                // Unable to open connection to this address. Keep trying with
                // the next address.
                DBG("Asynchronous failure");
                timeout_.pop(tmoHandler);
                closeSock();
                openSocket(rp->ai_next);
              } else {
                // Try to log in. That could take a while. So, again, let's
                // run this operation asynchronously. If everything worked, we
                // are done. But if we fail to log in then try the next address.
                timeout_.pop(tmoHandler);
                doLogin();
              }
              return true;
            });
            return;
          } else {
            DBG("Unexpected I/O error: " << errno);
            timeout_.pop(tmoHandler);
            closeSock();
          }
        }
        // Synchronously iterate to the next address. This is an unlikely
        // code path, as we won't get here if any of the I/O operations
        // blocked.
        rp = rp->ai_next;
      }
    });
    openSocket(result);
  };

  if (!gateway_.empty()) {
    // If the caller provided the address or name of the server, we can
    // connect directly.
    lookupAddress(gateway_);
  } else {
    // If the address isn't known, use a helper script to scan the network.
    // This operation can take a while, so perform it asynchronously.
    FILE *fp = popen("./find-radiora2.sh", "r");
    const auto pcloseHandler = timeout_.push([event = &event_, fp]() {
      event->removePollFd(fileno(fp));
      pclose(fp);
    });
    g_ = "";
    event_.addPollFd(fileno(fp), POLLIN, [=, this, &gateway = this->g_](auto) {
      char buf[64];
      const auto rc = read(fileno(fp), buf, sizeof(buf));
      // If we reached the end of the file, use this as the server to
      // connect to.
      if (rc <= 0) {
        timeout_.pop(pcloseHandler);
        lookupAddress(gateway = Util::trim(gateway));
        return false;
      } else {
        gateway = gateway + std::string(buf, rc);
      }
      return true;
    });
  }
}

void Lutron::enterPassword(std::function<void (void)> cb,
                           std::function<void (void)> err) {
  if (atPrompt_) {
    // Something is horribly wrong. We should never try to enter
    // credentials when we are already at the "GNET> " prompt.
    closeSock();
    err();
    return;
  }
  // Enter credentials when prompted, then wait for the normal
  // "GNET> " prompt.
  sendData(username_ + "\r\n", [=, this]() {
  sendData(passwd_ + "\r\n", [=, this]() {
  waitForPrompt(PROMPT, cb, err, false); },
           err, "password: ", false); },
           err, "login: ", false);
}

void Lutron::processLine(const std::string& line) {
  // This method does the heavy lifting. The Lutron wire protocol has a
  // few warts, especially with regards to error handling. All read lines
  // and prompts will be forwarded to this method and it looks at our
  // current state to decide which callbacks to invoke.
  //  - "later_" is a vector of commands that were submitted while there
  //    already was another command pending. We should work through this
  //    list as we finish other commands. Lutron::checkDelayed() does this
  //    for us.
  //  - "pending_" is a vector of commands that have already been submitted
  //    but not completed. It shouldn't have more than at most one entry.
  //  - "onPrompt_" is a vector of callbacks that should run when we see the
  //    "GNET> " prompt. This allows us to deal with commands that might or
  //    might not have a status code (i.e. ERROR or returned value from query).
  if (line == PROMPT) {
    // We saw the "GNET> " prompt. All pending commands are now done. A command
    // might have completed earlier, if it received a non-void return code.
    // These commands will push their callback onto the "onPrompt_" vector.
    atPrompt_ = true;
    if (!inCallback_) {
      timeout_.clear();
    }
    // Any previous commands that were waiting for a new command prompt are
    // now done.
    const auto onPrompt = std::move(onPrompt_);
    for (const auto& completion : onPrompt) {
      event_.runLater(completion);
    }
    // If we are still in the process of executing a query command, but now
    // saw a prompt command instead, assume that there won't be a result code.
    while (inCommand_ && pending_[inCallback_].size()) {
      const auto cmd = *pending_[inCallback_].begin();
      pending_[inCallback_].erase(pending_[inCallback_].begin());
      event_.runLater([=, cb = std::get<1>(cmd)]() { cb(""); });
    }
    inCommand_ = false;
    checkDelayed();
    readLine();
  } else if (pending_[inCallback_].size() &&
             line == std::get<0>(*pending_[inCallback_].rbegin())) {
    // Other than the "GNET> " prompt, there also are "login: " and
    // "password: " prompts. They only require calling the callback.
    const auto cb = std::get<1>(*pending_[inCallback_].rbegin());
    pending_[inCallback_].pop_back();
    event_.runLater([=]() { cb(line); });
  } else if (Util::starts_with(line, "~ERROR") ||
             line == "is an unknown command") {
    if (!inCallback_) {
      timeout_.clear();
    }
    // Lutron doesn't always send an error message, when things go wrong.
    // And it also has two different formats for error messages. We do our
    // best to line up error messages with the command that triggered them.
    if (inCommand_ && pending_[inCallback_].size()) {
      const auto cmd = *pending_[inCallback_].begin();
      pending_[inCallback_].erase(pending_[inCallback_].begin());
      DBG("Found error message; command \"" << std::get<0>(cmd) << "\"");
      onPrompt_.push_back(std::get<2>(cmd));
    }
  } else if (Util::starts_with(line, "~")) {
    // Command starting with "~" character signal a status change. This could
    // be the response to a query "?" command, or it could be an unsolicted
    // update. We make a best effort to find the matching command, if there is
    // one.
    for (auto it = pending_[inCallback_].begin();
         it != pending_[inCallback_].end();) {
      const auto& pending = std::get<0>(*it);
      if (pending.size() > 1 &&
          Util::starts_with(line, "~" +
                            pending.substr(1, pending.find_last_of(',')-1))) {
        if (!inCallback_) {
          timeout_.clear();
        }
        onPrompt_.push_back([=, cb = std::get<1>(*it)]() { cb(line); });
        it = pending_[inCallback_].erase(it);
        break;
      } else {
        ++it;
      }
    }
  }
}

// Lutron::readLine() is event driven and listens for full lines of
// data from the socket. It then calls Lutron::processLine() to determine
// what the data means, and executes the different types of delayed
// operations (i.e. later_, pending_, or onPrompt_).
void Lutron::readLine() {
  // If we read an entire line earlier, return it now. Trim all newline
  // characters at front and back of string. As a special case, we also
  // recognize the command prompt and always return that as if it was
  // a complete line. For the purposes of this discussion "login: " and
  // "password: " are also treated as prompts.
  const std::string user =
    pending_[inCallback_].size() ?
    std::get<0>(*pending_[inCallback_].rbegin()) : "";
  const std::string SEP("\r\n", 3);
  const auto skip = std::min(ahead_.size(), ahead_.find_first_not_of(SEP));
  const auto gnet = ahead_.find(PROMPT, skip);
  if (gnet < ahead_.find_first_of(SEP, skip)) {
    if (!inCallback_) {
      // As long as we regularly see data, we assume that our connection
      // is still alive.
      advanceKeepAliveMonitor();
    }
  }
  // In the case of "login: " and "password: ", these special prompts are
  // stored in "user". If the variable is non-empty, scan the socket for
  // those strings.
  auto ws = std::min(gnet == std::string::npos ? gnet : gnet + 6,
                     ahead_.find_first_of(SEP, skip));
  if (!user.empty()) {
    const auto prompt = ahead_.find(user, skip);
    if (prompt != std::string::npos && prompt+user.size() < ws) {
      ws = prompt + user.size();
    }
  }
  if (ws != std::string::npos) {
    // Found a complete line in our buffer. Return it now and keep the
    // remainder of the buffered data, if any.
    std::string ret = ahead_.substr(skip, ws - skip);
    ahead_ = ahead_.substr(std::min(ahead_.size(),
                                    ahead_.find_first_not_of(SEP, ws)));
    if (input_) input_(ret != PROMPT ? ret : "");
    processLine(ret);
    readLine();
    return;
  }
  if (sock_ < 0) {
    // If the stream was closed already, return any buffered characters. No
    // need to scan for newline. But if there is no more unbuffered data,
    // return an error instead.
    if (skip >= ahead_.size()) {
      DBG("Lutron::readLine() -> ERROR");
      closeSock();
    } else {
      std::string ret = ahead_.substr(skip);
      ahead_.clear();
      if (input_) input_(ret != PROMPT ? ret : "");
      processLine(ret);
      readLine();
    }
    return;
  }
  // If we don't have enough data for a full line just yet, read more bytes
  // from the stream and then try again.
  event_.removePollFd(sock_);
  event_.addPollFd(sock_, POLLIN, [=, this](auto) {
    event_.removePollFd(sock_);
    char buf[64];
    const auto rc = read(sock_, buf, sizeof(buf));
    if (rc <= 0) {
      // Either end of stream or any other error makes us close the socket.
      // Please note the "isConnected_" does not yet get reset here.
      close(sock_);
      sock_ = -1;
    } else {
      ahead_ = ahead_ + std::string(buf, rc);
    }
    readLine();
    return true;
  });
}

void Lutron::pollIn(std::function<bool (pollfd *)> cb) {
  // If we already have buffered data, we can invoke the callback directly.
  // Otherwise inform the event loop. No need evaluate the return code from
  // the callback. Our code always returns false, indicating that the event
  // was a one-shot and should unregister itself.
  if (ahead_.empty() && sock_ >= 0) {
    event_.addPollFd(sock_, POLLIN, cb);
  } else {
    cb(nullptr);
  }
}

void Lutron::advanceKeepAliveMonitor() {
  // Every time we see a "GNET> " prompt, the keep alive timer gets bumped.
  // But if it ever manages to fire, we close the socket. It won't be
  // reopened until someone submits another command.
  if (keepAlive_) {
    event_.removeTimeout(keepAlive_);
    keepAlive_ = 0;
  }
  if (sock_ >= 0) {
    keepAlive_ = event_.addTimeout(KEEPALIVE, [this]() {
      keepAlive_ = 0;
      if (!commandPending()) {
        if (write(sock_, "\r\n", 2) != 2) {
          DBG("This is weird. Kernel started throttling TCP packages");
          advanceKeepAliveMonitor();
        } else {
          keepAlive_ = event_.addTimeout(KEEPALIVE, [this]() {
            DBG("Keep-alive expired");
            closeSock();
          });
        }
      } else {
        advanceKeepAliveMonitor();
      }
    });
  }
}

void Lutron::Timeout::set(unsigned tmo, std::function<void (void)> cb) {
  // Sets up a new timeout handler for running commands. This also allows us
  // to keep track of finalizers that have to execute when unwinding the
  // callstack. We can't do this using normal C++ destructors, as a chain
  // of event-driven callbacks isn't the same as a sequence of nest block
  // scopes.
  handle_ = event_.addTimeout(tmo, [=, this]() {
    clear();
    cb();
  });
}

void Lutron::Timeout::clear() {
  // Upon timeout, this method gets called twice. First we get called
  // before invoking the callback associated with the timeout. This is
  // necessary, as we want finalizers to execute before the callback.
  // But then the callback closes the socket, and the closeSock() calls
  // us a second time. That's fine. The code is deliberately written so
  // that it is safe to call clear() as many times as desired.
  if (handle_) {
    event_.removeTimeout(handle_);
  }
  // Execute all finalizers. They have to run before the timeout handling
  // can do its thing.
  pop();
  // Reset state. This makes sure we don't run any timeout related code
  // multiple times. It also ensures that isArmed() returns the correct
  // value.
  self_ = 0;
  handle_ = 0;
}

unsigned Lutron::Timeout::push(std::function<void (void)> cb) {
  // Adds a new finalizer and returns a unique identifier. The stack of
  // finalizers runs in LIFO order before the timeout handler does its
  // thing or whenever one or more entries are popped from the stack of
  // finalizers.
  finalizers_.push_back(std::make_pair(self_ = next(), cb));
  return self_;
}

void Lutron::Timeout::pop(unsigned id) {
  // Removes entries from the stack of finalizers until we encounter the
  // one that is specified by the "id". All popped finalizers will get
  // executed.
  for (bool done = false; !done && !finalizers_.empty(); ) {
    finalizers_.back().second();
    done = finalizers_.back().first == id;
    finalizers_.pop_back();
  }
}
