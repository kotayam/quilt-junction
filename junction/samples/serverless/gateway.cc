#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdio>
#include <ios>
#include <iostream>
#include <thread>

constexpr int GATEWAY_PORT = 8080;
constexpr const char *FUNCTION_IP = "10.10.1.3";
constexpr int FUNCTION_PORT = 43;

namespace {

int gw_fd;
sockaddr_in gw_addr;
int addrlen;
int func_fd;

bool InitGateway() {
  gw_fd = socket(AF_INET, SOCK_STREAM, 0);
  if (gw_fd < 0) {
    std::cerr << "Failed to create socket\n";
    return false;
  }

  // int opt = 1;
  // if (setsockopt(gw_fd, SOL_SOCKET, SO_REUSEADDR | SO_REUSEPORT, &opt,
  //                sizeof(opt)) < 0) {
  //   std::cerr << "Failed to set socket options\n";
  //   return false;
  // }

  gw_addr.sin_family = AF_INET;
  gw_addr.sin_addr.s_addr = INADDR_ANY;
  gw_addr.sin_port = htons(GATEWAY_PORT);
  addrlen = sizeof(gw_addr);

  if (bind(gw_fd, reinterpret_cast<sockaddr *>(&gw_addr), addrlen) < 0) {
    std::cerr << "Failed to bind socket\n";
    return false;
  }

  if (listen(gw_fd, 3) < 0) {
    std::cerr << "Failed to listen\n";
    return false;
  }

  std::cout << std::unitbuf << "Standard Gateway listening on port "
            << GATEWAY_PORT << "\n";
  return true;
}

bool ConnectToFunctionServer() {
  func_fd = socket(AF_INET, SOCK_STREAM, 0);
  if (func_fd < 0) {
    std::cerr << "Failed to create socket with function server\n";
    return false;
  }

  sockaddr_in server_addr;
  server_addr.sin_family = AF_INET;
  server_addr.sin_port = htons(FUNCTION_PORT);
  if (inet_pton(AF_INET, FUNCTION_IP, &server_addr.sin_addr) <= 0) {
    std::cerr << "Failed to set function server IP address\n";
    close(func_fd);
    return false;
  }

  std::cout << std::unitbuf << "Connecting to " << FUNCTION_IP << ":"
            << FUNCTION_PORT << "...\n";
  if (connect(func_fd, reinterpret_cast<sockaddr *>(&server_addr),
              sizeof(server_addr)) < 0) {
    std::cerr << "Failed to connect to function server\n";
    close(func_fd);
    return false;
  }

  return true;
}

void ProcessRequest(int client_fd) {
  char buffer[4096];
  ssize_t n = read(client_fd, buffer, sizeof(buffer) - 1);
  if (n <= 0) {
    close(client_fd);
    return;
  }
  buffer[n] = '\0';
  // strip new line character at end
  if (buffer[n - 1] == '\n') {
    buffer[n - 1] = '\0';
    n--;
  }

  std::cout << std::unitbuf << "[Gateway] Received: " << buffer << " (" << n
            << " bytes) from client.\n";

  if (!ConnectToFunctionServer()) {
    close(client_fd);
    return;
  }

  // write request to function server
  if (write(func_fd, buffer, n) < 0) {
    std::cerr << "[Gateway] Failed to write to function server\n";
    close(func_fd);
    close(client_fd);
    return;
  }

  // read response from function server
  n = read(func_fd, buffer, sizeof(buffer));
  if (n > 0) {
    // send response to client
    if (write(client_fd, buffer, n) < 0) {
      std::cerr << "[Gateway] Failed to write back to client\n";
      close(func_fd);
      close(client_fd);
      return;
    }
    std::cout << std::unitbuf << "[Gateway] Forwarded response to client.\n";
  }

  close(func_fd);
  close(client_fd);
}

void HandleRequest() {
  while (true) {
    int new_socket = accept(gw_fd, reinterpret_cast<sockaddr *>(&gw_addr),
                            reinterpret_cast<socklen_t *>(&addrlen));
    if (new_socket < 0) {
      std::cerr << "Failed to accept new socket\n";
      continue;
    }

    std::thread(ProcessRequest, new_socket).detach();
  }
}
}  // namespace

int main() {
  if (!InitGateway()) { return 1; }

  HandleRequest();

  return 0;
}
