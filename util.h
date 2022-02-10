#pragma once

#include <algorithm>
#include <cctype>
#include <functional>
#include <string>

#if defined(NDEBUG)
#define DBG(x)     do { } while (0)
#define DBGc(c, x) do { } while (0)
#else
extern "C" {
  int isatty(int);
}
#include <fmt/format.h>
#include <iostream>
#define DBGc(c, x) do { \
    static const bool tty = isatty(2); \
    unsigned ts = Util::dt(); \
    std::cerr << fmt::format("{:3}.{:03}: ", ts/1000, ts%1000) \
              << ((tty && c) ? fmt::format("\x1B[{}m", 30 + c) : "") << x \
              << ((tty && c) ? "\x1B[m" : "") \
              << std::endl; \
  } while (0)
#define DBG(x) DBGc(0, x)
#endif


namespace Util {
  unsigned int millis();
  unsigned int micros();
  unsigned int timeOfDay();

  inline std::string trim(const std::string& s) {
    auto wsfront = std::find_if_not(s.begin(), s.end(),
                                    [](int c) { return std::isspace(c); });
    auto wsback = std::find_if_not(s.rbegin(), s.rend(),
                                   [](int c) { return std::isspace(c);}).base();
    return (wsback <= wsfront ? std::string() : std::string(wsfront, wsback));
  }


  inline bool starts_with(const std::string& s, const std::string starts) {
    return !s.rfind(starts, 0);
  }

  inline bool ends_with(const std::string& s, const std::string& ends) {
    return ends.size() <= s.size() &&
           std::equal(ends.rbegin(), ends.rend(), s.rbegin());
  }

  inline unsigned dt() {
    static unsigned last = millis();
    unsigned now = millis();
    unsigned delta = now - last;
    last = now;
    return delta;
  }

  template <class F>
  struct recursive_ {
    F f;
    template <class... Ts>
    decltype(auto) operator()(Ts&&... ts) const {
      return f(*this, std::forward<Ts>(ts)...); }
    template <class... Ts>
    decltype(auto) operator()(Ts&&... ts) {
      return f(*this, std::forward<Ts>(ts)...); }
  };
  template <class F> recursive_(F) -> recursive_<F>;
  static auto const rec = [](auto f){ return recursive_{std::move(f)}; };
};
