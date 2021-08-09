#pragma once

#include "event.h"
#include "libwebsockets.h"


class WS {
 public:
  WS(Event *event, int port = 80);
  ~WS();

 private:
  Event *event_;
  void *loop_;
  void *loops_;
  lws_context *ctx_;
  lws_event_loop_ops ops_;
  lws_plugin_evlib_t evlib_;
  lws_http_mount mount_;
  lws_context_creation_info info_;

  static int init(lws_context *ctx, void *_loop, int tsi);
  static int accept(lws *wsi);
  static void io(lws *wsi, unsigned flags);
  static int close(lws *wsi);
};
