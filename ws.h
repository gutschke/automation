#pragma once

#include <functional>
#include <map>
#include <string>

#include "event.h"
#include "libwebsockets.h"


class WS {
 public:
  WS(Event *event, int port = 80,
     std::function<const std::string ()> keypads = nullptr,
     std::function<void (const std::string&)> cmd = nullptr);
  ~WS();
  void broadcast(const std::string& s);

 private:
  Event *event_;
  std::function<const std::string ()> keypads_;
  std::function<void (const std::string&)> cmd_;
  void *loop_;
  void *loops_;
  lws_context *ctx_;
  lws_event_loop_ops ops_;
  lws_plugin_evlib_t evlib_;
  lws_http_mount mount_[2];
  lws_context_creation_info info_;
  lws_protocols protocols_[4];
  lws_protocol_vhost_options headers_[5];
  std::map<lws *, std::string *> wsi_;

  static int init(lws_context *ctx, void *_loop, int tsi);
  static int accept(lws *wsi);
  static void io(lws *wsi, unsigned flags);
  static int close(lws *wsi);
  static int keypadsCallback(lws *wsi, lws_callback_reasons reason,
                             void *user, void *in, size_t len);
  static int websocketCallback(lws *wsi, lws_callback_reasons reason,
                               void *user, void *in, size_t len);
};
