#pragma once

#include <algorithm>
#include <cctype>
#include <functional>
#include <string>

#if defined(NDEBUG)
#define DBG(x) do { } while (0)
#else
#include <iostream>
#define DBG(x) do { std::cerr << x << std::endl; } while (0)
#endif


namespace Util {
  unsigned int millis();
  unsigned int micros();

  inline std::string trim(const std::string& s) {
    auto wsfront = std::find_if_not(s.begin(), s.end(),
                                    [](int c) { return std::isspace(c); });
    auto wsback = std::find_if_not(s.rbegin(), s.rend(),
                                   [](int c) { return std::isspace(c);}).base();
    return (wsback <= wsfront ? std::string() : std::string(wsfront, wsback));
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
