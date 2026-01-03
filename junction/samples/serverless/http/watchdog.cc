#include "watchdog.h"

#include <iostream>
#include <string>

#include "lib/httplib.h"

constexpr const char *SOCK_PATH = "/tmp/serverless/";
constexpr const char *SOCK_EXT = ".sock";

WatchDog::WatchDog(const std::string &name, httplib::Server::Handler h)
    : handler_(std::move(h)) {
  sock_path_ = SOCK_PATH + name + SOCK_EXT;
}

bool WatchDog::InitServer() {
  httplib::Server svr;
  svr.Get(".*", handler_);

  if (!svr.set_address_family(AF_INET).listen(sock_path_, 80)) {
    std::cerr << "[Watchdog] Failed to listen on " << sock_path_ << std::endl;
    return false;
  }

  return true;
}

void WatchDog::Run() {
  if (!InitServer()) { exit(1); }
}
