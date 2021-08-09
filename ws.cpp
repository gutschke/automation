#include "util.h"
#include "ws.h"


WS::WS(Event* event, int port)
  : event_(event), loops_(this),
    ctx_{0}, ops_{0}, evlib_{0}, mount_{0}, info_{0} {
  loop_ = event_->addLoop([this](unsigned tmo) {
    tmo = std::max(1u, tmo);
    unsigned newTmo = lws_service_adjust_timeout(ctx_, tmo, 0);
    if (newTmo < tmo) {
//    DBG("libwebsocket is shortening timeout from " << tmo <<
//        "ms to " << newTmo << "ms");
      // Need to add a timeout to make poll() exit early and for the
      // event loop to call into this handler again. That will end up
      // periodically calling "lws_service_adjust_timeout()". But the
      // actual timeout handler can have an empty callback.
      event_->addTimeout(newTmo, [](){});
    }
  });
  ops_.name = "automation";
  ops_.init_pt = WS::init;
  ops_.init_vhost_listen_wsi = WS::accept;
  ops_.sock_accept = WS::accept;
  ops_.io = WS::io;
  ops_.wsi_logical_close = WS::close;
  ops_.evlib_size_pt = sizeof(WS *);
  evlib_.hdr.name = "automation";
  evlib_.hdr._class = "lws_evlib_plugin";
  evlib_.hdr.lws_build_hash = LWS_BUILD_HASH;
  evlib_.hdr.api_magic = LWS_PLUGIN_API_MAGIC;
  evlib_.ops = &ops_;
  mount_.mountpoint = "/";
  mount_.mountpoint_len = 1;
  mount_.origin = "www";
  mount_.def = "index.html";
  mount_.origin_protocol = LWSMPRO_FILE;
  info_.port = port;
  info_.mounts = &mount_;
  info_.error_document_404 = "/404.html";
  info_.options =
    LWS_SERVER_OPTION_HTTP_HEADERS_SECURITY_BEST_PRACTICES_ENFORCE;
  info_.event_lib_custom = &evlib_;
  info_.foreign_loops = &loops_;
  lws_create_context(&info_);
}

WS::~WS() {
  if (ctx_) {
    lws_context_destroy(ctx_);
  }
}

int WS::init(lws_context *ctx, void *_loop, int tsi) {
//DBG("WS::init()");
  (*(WS **)lws_evlib_tsi_to_evlib_pt(ctx, tsi) = (WS *)_loop)->ctx_ = ctx;
  return 0;
}

int WS::accept(lws *wsi) {
//DBG("WS::accept(" << lws_get_socket_fd(wsi) << ")");
  WS *ws = *(WS **)lws_evlib_wsi_to_evlib_pt(wsi);
  int fd = lws_get_socket_fd(wsi);
  ws->event_->addPollFd(fd, POLLIN,
    [ctx = ws->ctx_](pollfd *pfd) {
      lws_service_fd(ctx, pfd);
      return true;
    });
  return 0;
}

void WS::io(lws *wsi, unsigned flags) {
//DBG("WS::io(" << lws_get_socket_fd(wsi) << ", " <<
//    ((flags & LWS_EV_START) ? "START" : "STOP") <<
//    ((flags & LWS_EV_READ) ? ", READ" : "") <<
//    ((flags & LWS_EV_READ) ? ", WRITE" : "") << ")");
  WS *ws = *(WS **)lws_evlib_wsi_to_evlib_pt(wsi);
  if (flags & LWS_EV_READ) {
    ws->event_->removePollFd(lws_get_socket_fd(wsi), POLLIN);
  }
  if (flags & LWS_EV_WRITE) {
    ws->event_->removePollFd(lws_get_socket_fd(wsi), POLLOUT);
  }
  if (flags & LWS_EV_START) {
    if (flags & LWS_EV_READ) {
      ws->event_->addPollFd(lws_get_socket_fd(wsi), POLLIN,
        [ctx = ws->ctx_](pollfd *pfd) {
          lws_service_fd(ctx, pfd);
          return true;
        });
    }
    if (flags & LWS_EV_WRITE) {
      ws->event_->addPollFd(lws_get_socket_fd(wsi), POLLOUT,
        [ctx = ws->ctx_](pollfd *pfd) {
          lws_service_fd(ctx, pfd);
          return true;
        });
    }
  }
}

int WS::close(lws *wsi) {
  WS *ws = *(WS **)lws_evlib_wsi_to_evlib_pt(wsi);
//DBG("WS::close(" << lws_get_socket_fd(wsi) << ")");
  ws->event_->removePollFd(lws_get_socket_fd(wsi));
  return 0;
}
