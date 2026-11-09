#include "hm/coordinator.hpp"
#include <cstdlib>
#include <iostream>
#include <unistd.h>
int main()
{
  // A short-lived launcher is reaped by clients; the service survives app/MCP
  // shutdown.
  pid_t pid = fork();
  if (pid < 0)
    return 1;
  if (pid > 0)
    return 0;
  setsid();
  try {
    return hm::run_coordinator_service();
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
