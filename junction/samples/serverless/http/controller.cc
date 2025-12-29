#include "lib/httplib.h"

constexpr int CONTROLLER_PORT = 43;

namespace {

void GetUserHandler(const httplib::Request &req, httplib::Response &res) {
  // TODO: write request to socket and read back response
  res.set_content("Test", "text/plain");
}

void InitServer() {
  httplib::Server svr;
  svr.Get("/user", GetUserHandler);
  svr.listen("0.0.0.0", CONTROLLER_PORT);
}
}  // namespace

int main() {
  InitServer();
  return 0;
}
