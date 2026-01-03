#include <iostream>

#include "lib/httplib.h"

constexpr int CONTROLLER_PORT = 8080;

const std::string SOCK_PATH = "/tmp/serverless/";
const std::string USER_SOCK = "user.sock";
const std::string FOLLOWER_SOCK = "follower.sock";

namespace {

httplib::Server::Handler SocketHandler(std::string_view sock_path) {
  return [sock_path](const httplib::Request &req, httplib::Response &res) {
    httplib::Client cli(sock_path.data());
    cli.set_address_family(AF_INET);
    auto func_res = cli.Get(req.path, req.headers);
    if (func_res) {
      res.status = func_res->status;
      res.body = func_res->body;
      for (const auto &header : func_res->headers) {
        if (header.first != "Content-Length" &&
            header.first != "Transfer-Encoding") {
          res.set_header(header.first, header.second);
        }
      }
    } else {
      res.status = httplib::StatusCode::BadGateway_502;
      res.set_content("Bad Gateway: Function Unreachable", "text/plain");
    }
  };
}

bool InitServer() {
  httplib::Server svr;

  svr.Get("/user/:id", SocketHandler(SOCK_PATH + USER_SOCK));
  svr.Get("/followers/:id", SocketHandler(SOCK_PATH + FOLLOWER_SOCK));

  std::cout << "[Controller] Listening on " << "0.0.0.0" << ":"
            << CONTROLLER_PORT << std::endl;

  return svr.listen("0.0.0.0", CONTROLLER_PORT);
}
}  // namespace

int main() {
  if (!InitServer()) {
    std::cerr << "[Controller] Failed to listen on port: " << CONTROLLER_PORT
              << std::endl;
    exit(1);
  }
  return 0;
}
