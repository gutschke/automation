#include <time.h>

#include "util.h"

unsigned int Util::millis() {
  struct timespec spec;
  clock_gettime(CLOCK_MONOTONIC, &spec);
  return(spec.tv_sec*1000 + spec.tv_nsec / 1000000);
}

unsigned int Util::micros() {
  struct timespec spec;
  clock_gettime(CLOCK_MONOTONIC, &spec);
  return(spec.tv_sec*1000000 + spec.tv_nsec / 1000);
}
