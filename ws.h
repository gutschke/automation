#pragma once

#include "event.h"
#include "libwebsockets.h"


class WS {
 public:
  WS(Event *event, int port = 80,
     std::function<const std::string ()> keypads = nullptr);
  ~WS();

 private:
  struct KeypadsState {
    std::string data;
  };

  class Message {
  public:
    Message& operator=(const std::string& s) {
      if (initialized_) {
        using namespace std;
        msg.~string();
      }
      (new (&msg) std::string(LWS_PRE, '\000'))->append(s);
      initialized_ = true;
      return *this;
    }
    static void destroy(void *msg) {
      if (!((Message *)msg)->initialized_) {
        ((Message *)msg)->~Message();
      }
    }
    size_t size() { return msg.size() - LWS_PRE; }
    unsigned char *data() { return (unsigned char *)&msg[LWS_PRE]; }
  private:
    Message();
    ~Message() { initialized_ = false; };
    std::string msg;
    bool initialized_;
  };

  struct SessionState {
    lws          *wsi;
    SessionState *list;
    uint32_t     tail;
  };

  struct HostState {
    HostState()
      : ring(lws_ring_create(sizeof(Message), 50, Message::destroy)),
        sessions(nullptr) {
      // WARNING! This object is created with placement new and libwebsockets
      // will never call the destructor. Store POD types only.
    }
    lws_ring     *ring;
    SessionState *sessions;
  };

  Event *event_;
  std::function<const std::string ()> keypads_;
  void *loop_;
  void *loops_;
  lws_context *ctx_;
  lws_event_loop_ops ops_;
  lws_plugin_evlib_t evlib_;
  lws_http_mount mount_[2];
  lws_context_creation_info info_;
  lws_protocols protocols_[4];
  lws_protocol_vhost_options headers_[5];

  static int init(lws_context *ctx, void *_loop, int tsi);
  static int accept(lws *wsi);
  static void io(lws *wsi, unsigned flags);
  static int close(lws *wsi);
  static int keypadsCallback(lws *wsi, lws_callback_reasons reason,
                             void *user, void *in, size_t len);
  static int websocketCallback(lws *wsi, lws_callback_reasons reason,
                               void *user, void *in, size_t len);
};
