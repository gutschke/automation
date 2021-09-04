#include "util.h"
#include "ws.h"


WS::WS(Event* event, int port, std::function<const std::string ()> keypads,
       std::function<void (const std::string&)> cmd)
  : event_(event), keypads_(keypads), cmd_(cmd), loops_(this),
    ctx_{0}, ops_{0}, evlib_{0}, mount_{0}, info_{0}, protocols_{0},
    headers_{0} {
  // Make our event loop compatible with what libwebsocket wants.
  loop_ = event_->addLoop([this](unsigned tmo) {
    tmo = std::max(1u, tmo);
    unsigned newTmo = lws_service_adjust_timeout(ctx_, tmo, 0);
    if (newTmo == 0) {
      lws_service_tsi(ctx_, -1, 0);
    } else if (newTmo < tmo) {
      // Need to add a timeout to make poll() exit early and for the
      // event loop to call into this handler again. That will end up
      // periodically calling "lws_service_adjust_timeout()". But the
      // actual timeout handler can have an empty callback.
      event_->addTimeout(newTmo, [](){});
    }
  });

  // Register operations, so that the HTTP handler can talk to our event loop.
  ops_.name = "automation";
  ops_.init_pt = WS::init;
  ops_.init_vhost_listen_wsi = WS::accept;
  ops_.sock_accept = WS::accept;
  ops_.io = WS::io;
  ops_.wsi_logical_close = WS::close;
  ops_.evlib_size_pt = sizeof(WS *);

  // Register as a new plugin.
  evlib_.hdr.name = "automation";
  evlib_.hdr._class = "lws_evlib_plugin";
  evlib_.hdr.lws_build_hash = LWS_BUILD_HASH;
  evlib_.hdr.api_magic = LWS_PLUGIN_API_MAGIC;
  evlib_.ops = &ops_;

  // Set up the mountpoint where to find static files.
  mount_[0].mountpoint = "/";
  mount_[0].mountpoint_len = 1;
  mount_[0].origin = "www";
  mount_[0].def = "index.html";
  mount_[0].origin_protocol = LWSMPRO_FILE;
  mount_[0].mount_next = &mount_[1];
  mount_[1].mountpoint = "/keypads.json";
  mount_[1].mountpoint_len = 13;
  mount_[1].def = "";
  mount_[1].protocol = "keypads";
  mount_[1].origin_protocol = LWSMPRO_CALLBACK;

  // Configure supported protocols.
  protocols_[0].name = "http";
  protocols_[0].callback = lws_callback_http_dummy;
  protocols_[1].name = "keypads";
  protocols_[1].callback = keypadsCallback;
  protocols_[1].per_session_data_size = sizeof(KeypadsState);
  protocols_[2].name = "ws";
  protocols_[2].callback = websocketCallback;
  protocols_[2].per_session_data_size = sizeof(std::string *);
  protocols_[2].rx_buffer_size = 128;

  // Set global options for security headers.
  headers_[0].next = &headers_[1];
  headers_[0].name = "content-security-policy:";
  headers_[0].value = "default-src 'none'; img-src 'none'; "
                      "script-src 'unsafe-inline'; font-src 'none'; "
                      "style-src 'unsafe-inline'; connect-src 'self'; "
                      "frame-ancestors 'none'; base-uri 'none'; "
                      "form-action 'self'";
  headers_[1].next = &headers_[2];
  headers_[1].name = "x-content-type-options:";
  headers_[1].value = "nosniff";
  headers_[2].next = &headers_[3];
  headers_[2].name = "x-xss-protection:";
  headers_[2].value = "1; mode=block";
  headers_[3].next = &headers_[4];
  headers_[3].name = "x-frame-options:";
  headers_[3].value = "deny";
  headers_[4].name = "referrer-policy:";
  headers_[4].value = "no-referrer";

  // Configure web server settings.
  info_.port = port;
  info_.mounts = &mount_[0];
  info_.error_document_404 = "/404.html";
  info_.headers = headers_;
  info_.protocols = protocols_;
  info_.event_lib_custom = &evlib_;
  info_.foreign_loops = &loops_;
  info_.ka_time = 120;
  info_.ka_probes = 3;
  info_.ka_interval = 30;
  info_.user = this;

  // Now we are ready to create the web server.
  lws_create_context(&info_);
}

WS::~WS() {
  // Clean up all libwebsocket state and unregister from event loop.
  if (ctx_) {
    lws_context_destroy(ctx_);
  }
  event_->removeLoop(loop_);
}

int WS::init(lws_context *ctx, void *_loop, int tsi) {
  // Keep track of the context information, so that we can access it from
  // other callbacks.
  (*(WS **)lws_evlib_tsi_to_evlib_pt(ctx, tsi) = (WS *)_loop)->ctx_ = ctx;
  return 0;
}

int WS::accept(lws *wsi) {
  // Accept a new incoming connection and add the file descriptor to the
  // event loop. Call back into libwebsocket, whenever new data arrives.
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
  // libwebsocket calls this function whenever it wants to add or remove
  // event handlers for the event loop.
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
  // When a connection is closed, remove all descriptors from the event loop.
  WS *ws = *(WS **)lws_evlib_wsi_to_evlib_pt(wsi);
  ws->event_->removePollFd(lws_get_socket_fd(wsi));
  return 0;
}

int WS::keypadsCallback(lws *wsi, lws_callback_reasons reason,
                        void *user, void *in, size_t len) {
  WS *that = (WS *)lws_context_user(lws_get_context(wsi));
  auto *session = (KeypadsState *)user;
  uint8_t buf[LWS_PRE + 2048], *start = &buf[LWS_PRE], *ptr = start;
  uint8_t *end = &buf[sizeof(buf) - LWS_PRE - 1];

  switch (reason) {
  case LWS_CALLBACK_PROTOCOL_INIT:
    DBG("Keypads::LWS_CALLBACK_PROTOCOL_INIT");
    break;
  case LWS_CALLBACK_HTTP_BIND_PROTOCOL:
    DBG("Keypads::LWS_CALLBACK_HTTP_BIND_PROTOCOL");
    new (session) KeypadsState();
    break;
  case LWS_CALLBACK_HTTP:
    DBG("Keypads::LWS_CALLBACK_HTTP");
    if (lws_add_http_common_headers(wsi, HTTP_STATUS_OK, "application/json",
                                    LWS_ILLEGAL_HTTP_CONTENT_LEN, &ptr, end) ||
        lws_finalize_write_http_header(wsi, start, &ptr, end)) {
      return 1;
    }
    if (that->keypads_) {
      session->data = that->keypads_();
    }
    lws_callback_on_writable(wsi);
    return 0;
  case LWS_CALLBACK_HTTP_WRITEABLE: {
    DBG("Keypads::LWS_CALLBACK_HTTP_WRITEABLE");
    if (!session) {
      break;
    }
    size_t n = std::min(session->data.size(), lws_ptr_diff_size_t(end, ptr));
    const lws_write_protocol mode =
      (n == session->data.size()) ? LWS_WRITE_HTTP_FINAL : LWS_WRITE_HTTP;
    memcpy(ptr, session->data.data(), n);
    ptr += n;
    if (lws_write(wsi, start, n, mode) != (ssize_t)n) {
      return 1;
    } else {
      session->data.erase(0, n);
    }
    if (mode == LWS_WRITE_HTTP_FINAL) {
      if (lws_http_transaction_completed(wsi)) {
        return -1;
      }
    } else {
      lws_callback_on_writable(wsi);
    }
    return 0;
  }
  case LWS_CALLBACK_HTTP_DROP_PROTOCOL:
    DBG("Keypads::LWS_CALLBACK_HTTP_DROP_PROTOCOL");
    session->~KeypadsState();
    break;
  case LWS_CALLBACK_CLOSED_HTTP:
    DBG("Keypads::LWS_CALLBACK_CLOSED_HTTP");
    break;
  case LWS_CALLBACK_CHECK_ACCESS_RIGHTS:
//  DBG("Keypads::LWS_CALLBACK_CHECK_ACCESS_RIGHTS");
    break;
  case LWS_CALLBACK_EVENT_WAIT_CANCELLED:
//  DBG("Keypads::LWS_CALLBACK_EVENT_WAIT_CANCELLED");
    break;
  default:
    DBG("Keypads::Reason: " << reason);
    break;
  }
  return lws_callback_http_dummy(wsi, reason, user, in, len);
}

int WS::websocketCallback(lws *wsi, lws_callback_reasons reason,
                        void *user, void *in, size_t len) {
  WS *that = (WS *)lws_context_user(lws_get_context(wsi));
  auto pending = user ? *(std::string **)user : nullptr;

  switch (reason) {
  case LWS_CALLBACK_PROTOCOL_INIT:
    DBG("WebSocket::LWS_CALLBACK_PROTOCOL_INIT");
    break;
  case LWS_CALLBACK_PROTOCOL_DESTROY:
    DBG("WebSocket::LWS_CALLBACK_PROTOCOL_DESTROY");
    for (auto& [ wsi, pending ] : that->wsi_) {
      delete pending;
    }
    that->wsi_.clear();
    break;
  case LWS_CALLBACK_HTTP_BIND_PROTOCOL:
    DBG("WebSocket::LWS_CALLBACK_HTTP_BIND_PROTOCOL");
    break;
  case LWS_CALLBACK_FILTER_PROTOCOL_CONNECTION:
    DBG("WebSocket::LWS_CALLBACK_FILTER_PROTOCOL_CONNECTION");
    break;
  case LWS_CALLBACK_ADD_HEADERS:
    DBG("WebSocket::LWS_CALLBACK_ADD_HEADERS");
    break;
  case LWS_CALLBACK_ESTABLISHED:
    DBG("WebSocket::LWS_CALLBACK_ESTABLISHED");
    // New websocket opened connection to us and is waiting for updates.
    if (that->wsi_.find(wsi) == that->wsi_.end()) {
      that->wsi_[wsi] = *(std::string **)user = new std::string(LWS_PRE, 0);
    }
    break;
  case LWS_CALLBACK_CLOSED: {
    DBG("WebSocket::LWS_CALLBACK_CLOSED");
    // Remove closed websocket session from the list of live sessions.
    auto it = that->wsi_.find(wsi);
    if (it != that->wsi_.end()) {
      delete it->second;
      that->wsi_.erase(it);
      *(std::string **)user = nullptr;
    }
    break; }
  case LWS_CALLBACK_SERVER_WRITEABLE: {
    DBG("WebSocket::LWS_CALLBACK_SERVER_WRITEABLE");
    if (!pending || pending->size() <= LWS_PRE) {
      break;
    }
    if (lws_write(wsi, (unsigned char *)&(*pending)[LWS_PRE],
                  pending->size() - LWS_PRE, LWS_WRITE_TEXT)
        < (ssize_t)(pending->size() - LWS_PRE)) {
      return -1;
    }
    pending->erase(LWS_PRE);
    break; }
  case LWS_CALLBACK_RECEIVE: {
    DBG("WebSocket::LWS_CALLBACK_RECEIVE");
    const auto received = std::string((char *)in, len);
    DBG("\"" << received << "\"");
    if (that->cmd_ && received.size() > 0 && received[0] == '#') {
      that->cmd_(received);
    }
    break; }
  case LWS_CALLBACK_WS_PEER_INITIATED_CLOSE:
    DBG("WebSocket::LWS_CALLBACK_WS_PEER_INITIATED_CLOSE");
    break;
  case LWS_CALLBACK_WS_SERVER_DROP_PROTOCOL:
    DBG("WebSocket::LWS_CALLBACK_WS_SERVER_DROP_PROTOCOL");
    break;
  case LWS_CALLBACK_EVENT_WAIT_CANCELLED:
//  DBG("WebSocket::LWS_CALLBACK_EVENT_WAIT_CANCELLED");
    break;
  default:
    DBG("WebSocket::Reason: " << reason);
    break;
  }
  return 0;
}

void WS::broadcast(const std::string& s) {
// DBG("WebSocket::broadcast(\"" << s << "\")");
  // Newly enqueued data must be send to all listening clients when they
  // become writable.
  for (auto& [ wsi, pending ] : wsi_) {
    if (pending->size() > LWS_PRE) {
      pending->append(1, ' ');
    }
    pending->append(s);

    // Notify web socket.
    lws_callback_on_writable(wsi);
  }
}
