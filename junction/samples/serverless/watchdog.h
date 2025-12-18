#pragma once

#include <fstream>
#include <functional>
#include <mutex>
#include <string>

using RequestHandler = std::function<std::string(const std::string &method,
                                                 const std::string &path,
                                                 const std::string &body)>;

class WatchDog {
 public:
  explicit WatchDog(RequestHandler h);
  void Run();

 private:
  RequestHandler handler_;
  std::fstream channel_;
  std::mutex chan_mutex_;

  bool OpenChannel();
  bool Warmup();
  void Respond(const std::string &res);
  void Worker(const std::string &req);
  void ProcessRequest();
};
