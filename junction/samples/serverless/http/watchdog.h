#pragma once

#include <string>

#include "lib/httplib.h"

class WatchDog {
 public:
  explicit WatchDog(const std::string &name, httplib::Server::Handler h);
  void Run();

 private:
  std::string sock_path_;
  httplib::Server::Handler handler_;

  bool InitServer();
};
