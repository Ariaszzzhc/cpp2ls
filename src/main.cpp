#include <iostream>

#include "server.h"

int main() {
  cpp2ls::Server server{std::cin, std::cout};

  server.run();
  return 0;
}
