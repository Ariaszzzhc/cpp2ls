#include <iostream>

#include "server.h"

int main() {
  cpp2ls::server server{std::cin, std::cout};

  server.run();
  return 0;
}
