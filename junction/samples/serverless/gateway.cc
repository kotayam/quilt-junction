#include <netinet/in.h>
#include <sys/socket.h>

#include <iostream>

constexpr int GATEWAY_PORT = 8080;
constexpr const char *FUNCTION_IP = "192.168.127.7";
constexpr int FUNCTION_PORT = 43;

namespace {
int gw_fd;

bool InitGateway() {
  gw_fd = socket(AF_INET, SOCK_STREAM, 0);
  if (gw_fd < 0) {
    std::cerr << "Failed to create socket\n";
    return false;
  }

  int opt = 1;
  if (setsockopt(gw_fd, SOL_SOCKET, SO_REUSEADDR | SO_REUSEPORT, &opt,
                 sizeof(opt)) < 0) {
    std::cerr << "Failed to set socket options\n";
    return false;
  }

  sockaddr_in gw_addr;
  gw_addr.sin_family = AF_INET;
  gw_addr.sin_addr.s_addr = INADDR_ANY;
  gw_addr.sin_port = htons(GATEWAY_PORT);

  if (bind(gw_fd, reinterpret_cast<sockaddr *>(&gw_addr), sizeof(gw_addr)) <
      0) {
    std::cerr << "Failed to bind socket\n";
    return false;
  }

  if (listen(gw_fd, 3) < 0) {
    std::cerr << "Failed to listen\n";
    return false;
  }

  return true;
}

bool HandleRequest() {}
}  // namespace

int main() {
  if (!InitGateway()) { return 1; }

  return 0;
}
