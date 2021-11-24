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

unsigned int Util::timeOfDay() {
  time_t t = time(NULL);
  struct tm tm = { 0 };
  localtime_r(&t, &tm);
  return tm.tm_hour*100 + tm.tm_min;
}
