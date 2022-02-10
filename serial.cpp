#include <asm/fcntl.h>
#include <asm/termios.h>

#include "serial.h"
#include "util.h"


// glibc goes out of its way to make it hard to set custom baud rates.
// We have to include kernel header files and make direct ioctl()s. These
// header files conflict with the glibc versions. So, all of this code has
// to live in its own compilation unit, and we need to import the small
// number of glibc symbols that we need without actually using system headers.
extern "C" {
  int open(const char *, int, ...);
  int ioctl(int, unsigned long, ...);
  #ifndef __useconds_t_defined
  int usleep(unsigned);
  #else
  int usleep(__useconds_t);
  #endif
}

int Serial::open(const char *s) {
  int fd = ::open(s, O_RDWR|O_NOCTTY|O_CLOEXEC|O_NONBLOCK|O_SYNC);
  if (fd < 0) {
    return -1;
  }
  // DMX uses 8N2 at 250,000 baud. We don't care about the receiver, but
  // we have to explicitly turn on the transmitter by pulling RTS to low.
  struct termios2 opt;
  ioctl(fd, TCGETS2, &opt);
  opt.c_iflag &= ~(IGNBRK|BRKINT|PARMRK|ISTRIP|INLCR|IGNCR|ICRNL|IXON);
  opt.c_oflag &= ~(OPOST);
  opt.c_lflag &= ~(ECHO|ECHONL|ICANON|ISIG|IEXTEN);
  opt.c_cflag &= ~(CBAUD|CSIZE|PARENB|CRTSCTS);
  opt.c_cflag |=   BOTHER|CS8|CLOCAL|CSTOPB;
  opt.c_ispeed =
  opt.c_ospeed = 250000;
  ioctl(fd, TCSETS2, &opt);
  ioctl(fd, TIOCCBRK, 0);
  // Clear RTS to enable the output.
  int status;
  ioctl(fd, TIOCMGET, &status);
  status &= ~TIOCM_RTS;
  ioctl(fd, TIOCMSET, &status);
  return fd;
}

void Serial::brk(int fd) {
  // Drain output buffers before sending break.
  ioctl(fd, TCSBRK, (void *)1);
  // Time between breaks should be at least 1204µs.
  const auto now = Util::micros();
  static decltype(now) last = 0;
  if (last && (now - last) < 1204U) {
    usleep(1204 - (now - last));
  }
  // Send a break that is at least 92µs long, and that is followed by a
  // MAB (make-after-break) of at least 12µs.
  ioctl(fd, TIOCSBRK, 0);
  usleep(92);
  ioctl(fd, TIOCCBRK, 0);
  usleep(12);
}
