#include "gateway_client.h"

#include "lib/httplib.h"

constexpr int GATEWAY_PORT = 8080;
constexpr const char *GATEWAY_IP = "10.10.1.1";
const std::string USER_SOCK = "/tmp/serverless/user.sock";

/**
 * @brief Call the gateway from a function when there is a function-to-function
 * invocation.
 *
 * @param req
 * @return response
 */
std::string CallGateway(std::string_view req) {
  static thread_local httplib::Client cli(GATEWAY_IP, GATEWAY_PORT);
  cli.set_keep_alive(true);
  auto res = cli.Get(std::string(req));
  return res->body;
}
