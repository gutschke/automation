#include <cstdlib>
#include <iostream>

#include "relay.h"

int main(int argc, char *argv[]) {
  Event ev;
  Relay relay(ev);
  switch (argc) {
  case 2:
    std::cout << relay.get(atoi(argv[1])) << std::endl;
    break;
  case 3:
    relay.set(atoi(argv[1]), *argv[2] != '0');
    break;
  default:
    std::cout << "Usage: " << argv[0] << " [pin] {[val]}" << std::endl;
    break;
  }
  return 0;
}
