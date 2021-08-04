#pragma once

class Serial {
 public:
  static int open(const char *s);
  static void brk(int fd);
};
