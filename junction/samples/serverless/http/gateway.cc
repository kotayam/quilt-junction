#include <algorithm>
#include <iostream>
#include <vector>

#include "lib/httplib.h"

constexpr const char *GATEWAY_IP = "0.0.0.";
constexpr int GATEWAY_PORT = 8080;
constexpr const char *CONTROLLER_IP = "10.10.1.2";
constexpr int CONTROLLER_PORT = 43;

struct RequestRoute {
  std::string method;
  std::string path_prefix;
};

const std::vector<RequestRoute> ALLOWED_ROUTES = {
    {"GET", "/user"}, {"POST", "/user"}, {"GET", "/followers"}};

namespace {

bool IsValidRequestRoute(std::string_view method, std::string_view path) {
  return std::any_of(ALLOWED_ROUTES.begin(), ALLOWED_ROUTES.end(),
                     [method, path](const RequestRoute &r) {
                       return r.method == method &&
                              path.find(r.path_prefix) == 0;
                     });
}

void ProxyHandler(const httplib::Request &req, httplib::Response &res) {
  if (!IsValidRequestRoute(req.method, req.path)) {
    std::cout << "[Gateway] BLOCKED: " << req.method << " " << req.path
              << std::endl;
    res.status = httplib::StatusCode::NotFound_404;
    res.set_content("Route not found or method not allowed", "text/plain");
    return;
  }

  std::cout << std::unitbuf << "[Gateway] Forwarding " << req.method << " "
            << req.path << std::endl;
}

}  // namespace

int main() {
  httplib::Server svr;

  svr.Get(".*", ProxyHandler);
  svr.Post(".*", ProxyHandler);
  svr.Put(".*", ProxyHandler);
  svr.Delete(".*", ProxyHandler);

  std::cout << "[Gateway] Listening on " << GATEWAY_IP << ":" << GATEWAY_PORT
            << std::endl;
  svr.listen(GATEWAY_IP, GATEWAY_PORT);

  return 0;
}
